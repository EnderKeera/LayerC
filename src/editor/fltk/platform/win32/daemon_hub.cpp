#include "platform/win32/daemon_hub.hpp"

#include "daemon_process.hpp"
#include "live_edit_session.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace layerc::editor {
namespace {
using Clock = std::chrono::steady_clock;
using layerc::core::LiveMessage;
using layerc::core::LiveMessageType;
using layerc::core::ProfileRevision;

constexpr auto minimum_connect_wait = std::chrono::milliseconds(25);
// The worker owns every outcome, including the reload-signal fallback, and
// resolves each request by its deadline. A caller waits this much longer so it
// never races the worker for the result; it only gives up on a stuck worker.
constexpr auto caller_backstop_grace = std::chrono::milliseconds(500);

std::string win32_reason(DWORD error);

struct Request final {
  enum class Kind { status, reload };

  Request(Kind request_kind, std::optional<CommittedProfile> profile,
          const Clock::time_point request_deadline)
      : kind(request_kind), committed(std::move(profile)),
        deadline(request_deadline) {}

  Kind kind;
  std::optional<CommittedProfile> committed;
  Clock::time_point deadline;
  std::condition_variable ready;
  bool completed{};
  DaemonStatus status_result;
  ReloadResult reload_result;
  std::atomic<bool> request_sent{};
};

} // namespace

struct DaemonHub::State {
  State() {
    wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_event == nullptr) {
      const DWORD error = GetLastError();
      initialization_error = "The LayerD link wake event could not be created (" +
                             win32_reason(error) + ").";
    }
  }
  ~State() {
    if (wake_event != nullptr) CloseHandle(wake_event);
  }

  std::mutex mutex;
  std::mutex lifecycle_mutex;
  std::deque<std::shared_ptr<Request>> queue;
  std::shared_ptr<Request> active;
  std::thread worker;
  HANDLE wake_event{};
  bool stopping{};
  bool worker_failed{};
  std::string initialization_error;
  std::string worker_error;
};

namespace {

struct WaitResult {
  enum class Kind { wake, pipe, timeout, failed } kind{Kind::failed};
  DWORD error{};
};

DWORD win32_timeout(const std::chrono::milliseconds timeout) noexcept {
  if (timeout.count() <= 0) return 0;
  constexpr auto maximum = static_cast<std::int64_t>(INFINITE - 1U);
  return static_cast<DWORD>(std::min(timeout.count(), maximum));
}

DWORD remaining_timeout(const Clock::time_point deadline) noexcept {
  const auto now = Clock::now();
  if (now >= deadline) return 0;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - now);
  if (remaining.count() <= 0) return 0;
  constexpr auto maximum = static_cast<std::int64_t>(INFINITE - 1U);
  return static_cast<DWORD>(std::min(remaining.count(), maximum));
}

std::string win32_reason(const DWORD error) {
  return "Windows error " + std::to_string(error);
}

std::string pipe_unreachable_reason(const DWORD error) {
  if (error == ERROR_SEM_TIMEOUT || error == ERROR_PIPE_BUSY)
    return "LayerD live pipe is busy.";
  return "LayerD live pipe could not be reached (" + win32_reason(error) + ").";
}

bool call_service(const std::function<bool()> &service) noexcept {
  if (!service) return false;
  try {
    return service();
  } catch (...) {
    return false;
  }
}

void finish_status(const std::shared_ptr<DaemonHub::State> &state,
                   const std::shared_ptr<Request> &request,
                   DaemonStatus result) {
  std::lock_guard lock(state->mutex);
  if (request->completed) return;
  request->status_result = std::move(result);
  request->completed = true;
  request->ready.notify_all();
}

void finish_reload(const std::shared_ptr<DaemonHub::State> &state,
                   const std::shared_ptr<Request> &request,
                   ReloadResult result) {
  std::lock_guard lock(state->mutex);
  if (request->completed) return;
  request->reload_result = std::move(result);
  request->completed = true;
  request->ready.notify_all();
}

bool request_completed(const std::shared_ptr<DaemonHub::State> &state,
                       const std::shared_ptr<Request> &request) {
  std::lock_guard lock(state->mutex);
  return state->stopping || request->completed;
}

void clear_active(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request) {
  std::lock_guard lock(state->mutex);
  if (state->active == request) state->active.reset();
}

WaitResult wait_for_work(const std::shared_ptr<DaemonHub::State> &state,
                         PipeLiveEditTransport *transport,
                         const DWORD timeout) noexcept {
  if (state->wake_event == nullptr)
    return {.kind = WaitResult::Kind::failed, .error = ERROR_INVALID_HANDLE};

  std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> handles{};
  handles[0] = state->wake_event;
  std::size_t count = 1;
  if (transport != nullptr) {
    count += transport->wait_handles(
        std::span<HANDLE>(handles).subspan(1));
  }
  const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(count),
                                               handles.data(), FALSE, timeout);
  if (result == WAIT_OBJECT_0) return {.kind = WaitResult::Kind::wake};
  if (result == WAIT_TIMEOUT) return {.kind = WaitResult::Kind::timeout};
  if (result >= WAIT_OBJECT_0 + 1U && result < WAIT_OBJECT_0 + count)
    return {.kind = WaitResult::Kind::pipe};
  return {.kind = WaitResult::Kind::failed, .error = GetLastError()};
}

bool poll_yielded(const LiveEditStep &step) noexcept {
  return !step.forwarded.empty() || !step.protocol_errors.empty() ||
         step.begin_ready.has_value() || step.pending_expired ||
         step.authenticating || step.authenticated_now ||
         step.authentication_rejected || step.session_acknowledged ||
         !step.anchors.empty() || step.ended.has_value() ||
         step.live_profile_due || step.timer_interval.has_value();
}

DaemonStatus disconnected_status(std::string reason) {
  DaemonStatus status;
  status.error = std::move(reason);
  return status;
}

void fail_unreachable(const std::shared_ptr<DaemonHub::State> &state,
                      const std::shared_ptr<Request> &request,
                      const DaemonHubEnvironment &environment,
                      std::string reason) {
  if (request->kind == Request::Kind::status) {
    finish_status(state, request, disconnected_status(std::move(reason)));
    return;
  }
  (void)call_service(environment.signal_reload);
  finish_reload(state, request,
                {ReloadState::failed,
                 "LayerD's profile reload could not be confirmed: " + reason});
}

void fail_closed(const std::shared_ptr<DaemonHub::State> &state,
                 const std::shared_ptr<Request> &request,
                 std::string reason) {
  if (request->kind == Request::Kind::status) {
    finish_status(state, request, disconnected_status(std::move(reason)));
    return;
  }
  finish_reload(state, request, {ReloadState::failed, std::move(reason)});
}

void fail_timeout(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request) {
  if (request->kind == Request::Kind::status) {
    DaemonStatus status;
    status.running = request->request_sent.load();
    status.error = status.running
                       ? "LayerD did not answer the status request before the deadline."
                       : "LayerD did not authenticate the live pipe before the deadline.";
    finish_status(state, request, std::move(status));
    return;
  }
  finish_reload(state, request,
                {ReloadState::failed,
                 "LayerD did not confirm the profile reload before the deadline."});
}

void fail_shutdown_locked(const std::shared_ptr<Request> &request) {
  if (request->completed) return;
  if (request->kind == Request::Kind::status) {
    request->status_result.error = "The LayerD link was shut down.";
  } else {
    request->reload_result = {ReloadState::failed,
                              "The LayerD link was shut down."};
  }
  request->completed = true;
  request->ready.notify_all();
}

// Ends the worker: every waiting caller gets the reason and later requests
// are refused with it instead of queueing behind a thread that is gone.
void fail_worker_locked(DaemonHub::State &state, std::string reason) {
  state.worker_failed = true;
  state.worker_error = std::move(reason);
  if (state.active) fail_shutdown_locked(state.active);
  for (const auto &request : state.queue) fail_shutdown_locked(request);
  state.queue.clear();
  state.active.reset();
}

void handle_absent(const std::shared_ptr<DaemonHub::State> &state,
                   const std::shared_ptr<Request> &request,
                   const DaemonHubEnvironment &environment) {
  if (request->kind == Request::Kind::status) {
    DaemonStatus status;
    status.socket_missing = true;
    finish_status(state, request, std::move(status));
    return;
  }
  const bool started = call_service(environment.start_daemon);
  finish_reload(state, request,
                {ReloadState::daemon_unavailable,
                 started ? "LayerD is starting; the reload was not confirmed."
                         : "LayerD is not running and could not be started."});
}

void handle_replied_status(const std::shared_ptr<DaemonHub::State> &state,
                           const std::shared_ptr<Request> &request,
                           const LiveMessage &message) {
  DaemonStatus status;
  status.running = true;
  status.payload_json = message.text;
  const auto parsed = layerc::runtime::parse_runtime_diagnostics_json(message.text);
  const auto *diagnostics =
      std::get_if<layerc::runtime::RuntimeDiagnostics>(&parsed);
  if (diagnostics == nullptr) {
    status.error = "LayerD returned invalid runtime diagnostics.";
  } else {
    status.version = diagnostics->package_version;
    status.sessions = diagnostics->sessions.size();
    for (const auto &service : diagnostics->services) {
      CapabilityLine capability;
      capability.name = std::string(
          layerc::runtime::capability_name(service.capability));
      switch (service.state) {
      case layerc::runtime::ServiceState::absent:
        capability.availability =
            layerc::runtime::CapabilityAvailability::inactive;
        break;
      case layerc::runtime::ServiceState::active:
        capability.availability = service.last_error.empty()
            ? layerc::runtime::CapabilityAvailability::active
            : layerc::runtime::CapabilityAvailability::degraded;
        break;
      case layerc::runtime::ServiceState::faulted:
        capability.availability =
            layerc::runtime::CapabilityAvailability::unavailable;
        break;
      }
      capability.reasons = service.last_error;
      status.capabilities.push_back(std::move(capability));
    }
  }
  finish_status(state, request, std::move(status));
}

bool handle_reply(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request,
                  const ProfileRevision expected_revision,
                  const LiveMessage &message) {
  if (message.revision != expected_revision) return false;
  if (request->kind == Request::Kind::status) {
    if (message.type == LiveMessageType::diagnostics_reply) {
      handle_replied_status(state, request, message);
      return true;
    }
    if (message.type == LiveMessageType::error) {
      DaemonStatus status;
      status.running = true;
      status.error = message.text;
      finish_status(state, request, std::move(status));
      return true;
    }
    return false;
  }
  if (message.type == LiveMessageType::acknowledgement) {
    finish_reload(state, request, {ReloadState::acknowledged, {}});
    return true;
  }
  if (message.type == LiveMessageType::error) {
    finish_reload(state, request, {ReloadState::failed, message.text});
    return true;
  }
  return false;
}

struct ActiveDemand {
  std::shared_ptr<Request> request;
  bool presence_probed{};
  ProfileRevision reply_revision{};
};

void discard_session(std::unique_ptr<LiveEditSession> &session,
                     PipeLiveEditTransport *&transport) noexcept {
  session.reset();
  transport = nullptr;
}

void process_worker(const std::shared_ptr<DaemonHub::State> &state,
                    const DaemonHubEnvironment &environment) {
  std::unique_ptr<LiveEditSession> session;
  PipeLiveEditTransport *transport{};
  std::optional<ActiveDemand> active;

  for (;;) {
    {
      std::lock_guard lock(state->mutex);
      if (state->stopping) break;
      if (!active && !state->queue.empty()) {
        auto request = std::move(state->queue.front());
        state->queue.pop_front();
        state->active = request;
        active.emplace(ActiveDemand{std::move(request)});
      }
    }

    if (!active) {
      if (session && transport != nullptr && session->connected()) {
        const auto waited = wait_for_work(state, transport, INFINITE);
        if (waited.kind == WaitResult::Kind::wake) continue;
        if (waited.kind == WaitResult::Kind::pipe) {
          (void)session->poll({});
          if (!session->connected()) discard_session(session, transport);
          continue;
        }
        if (waited.kind == WaitResult::Kind::failed)
          discard_session(session, transport);
        continue;
      }
      if (session) discard_session(session, transport);
      if (wait_for_work(state, nullptr, INFINITE).kind ==
          WaitResult::Kind::failed) {
        std::lock_guard lock(state->mutex);
        fail_worker_locked(*state, "The LayerD link worker could not wait for requests.");
        return;
      }
      continue;
    }

    const auto request = active->request;
    if (request_completed(state, request)) {
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (Clock::now() >= request->deadline) {
      if (request->request_sent.load()) {
        fail_timeout(state, request);
      } else if (request->kind == Request::Kind::status) {
        finish_status(state, request,
                      disconnected_status("LayerD did not authenticate the live pipe before the deadline."));
      } else {
        fail_unreachable(state, request, environment,
                         "LayerD did not authenticate the live pipe before the deadline.");
      }
      clear_active(state, request);
      active.reset();
      continue;
    }

    if (request->kind == Request::Kind::reload && !request->committed) {
      const bool signalled = call_service(environment.signal_reload);
      finish_reload(state, request,
                    signalled ? ReloadResult{ReloadState::signalled, {}}
                              : ReloadResult{ReloadState::daemon_unavailable,
                                             "LayerD's reload signal could not be delivered."});
      clear_active(state, request);
      active.reset();
      continue;
    }

    if (!active->presence_probed) {
      if (session && !session->connected()) discard_session(session, transport);
      if (!session) {
        if (environment.pipe_name.empty()) {
          fail_unreachable(state, request, environment,
                           "LayerD live pipe name is empty.");
          clear_active(state, request);
          active.reset();
          continue;
        }
        if (WaitNamedPipeW(environment.pipe_name.c_str(), 1) == FALSE) {
          const DWORD error = GetLastError();
          if (error == ERROR_FILE_NOT_FOUND) {
            handle_absent(state, request, environment);
          } else {
            fail_unreachable(state, request, environment,
                              pipe_unreachable_reason(error));
          }
          clear_active(state, request);
          active.reset();
          continue;
        }
        auto owned_transport = std::make_unique<PipeLiveEditTransport>(
            environment.pipe_name, environment.expected_server_process_id);
        transport = owned_transport.get();
        session = std::make_unique<LiveEditSession>(std::move(owned_transport));
        const auto seed = (GetTickCount64() << 16U) | GetCurrentProcessId();
        session->seed_revision(static_cast<ProfileRevision>(seed));
      }
      active->presence_probed = true;
    }

    const auto step = session->poll({});
    if (step.authentication_rejected) {
      fail_unreachable(state, request, environment,
                       "LayerD refused live-pipe package authentication.");
      discard_session(session, transport);
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (!step.protocol_errors.empty()) {
      const std::string reason = "LayerD sent an invalid live-edit protocol message.";
      if (request->request_sent.load())
        fail_closed(state, request, reason);
      else
        fail_unreachable(state, request, environment, reason);
      discard_session(session, transport);
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (!session->authenticated()) {
      if (!session->connected() && transport != nullptr &&
          !transport->last_error().empty()) {
        fail_unreachable(state, request, environment,
                         "LayerD live-pipe authentication failed.");
        discard_session(session, transport);
        clear_active(state, request);
        active.reset();
        continue;
      }
      if (poll_yielded(step)) continue;
      const auto timeout = std::min(
          remaining_timeout(request->deadline),
          win32_timeout(minimum_connect_wait));
      const auto waited = wait_for_work(state, transport, timeout);
      if (waited.kind == WaitResult::Kind::failed) {
        fail_unreachable(state, request, environment,
                         "LayerD live-pipe connection wait failed (" +
                             win32_reason(waited.error) + ").");
        discard_session(session, transport);
        clear_active(state, request);
        active.reset();
      }
      continue;
    }

    if (!request->request_sent.load()) {
      bool sent{};
      if (request->kind == Request::Kind::status) {
        sent = session->send_authenticated(
            LiveMessage{.type = LiveMessageType::diagnostics_request});
      } else {
        const auto &committed = *request->committed;
        const auto key = layerc::platform::canonical_profile_key(committed.path);
        if (key.empty()) {
          fail_closed(state, request,
                      "The committed profile path could not be canonicalized.");
          clear_active(state, request);
          active.reset();
          continue;
        }
        sent = session->publish_committed(key, committed.json);
      }
      if (!sent) {
        if (!session->connected()) {
          fail_closed(state, request,
                      "LayerD closed the live pipe before the request was sent.");
        } else {
          fail_unreachable(state, request, environment,
                           "LayerD did not accept the live-pipe request.");
        }
        discard_session(session, transport);
        clear_active(state, request);
        active.reset();
        continue;
      }
      active->reply_revision = session->revision();
      request->request_sent.store(true);
      continue;
    }

    bool replied{};
    for (const auto &message : step.forwarded) {
      if (handle_reply(state, request, active->reply_revision, message)) {
        replied = true;
        break;
      }
    }
    if (replied || request_completed(state, request)) {
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (!session->connected()) {
      fail_closed(state, request,
                  "LayerD closed the live pipe before replying.");
      discard_session(session, transport);
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (poll_yielded(step)) continue;

    const auto waited = wait_for_work(
        state, transport, remaining_timeout(request->deadline));
    if (waited.kind == WaitResult::Kind::failed) {
      fail_closed(state, request,
                  "LayerD live-pipe wait failed (" +
                      win32_reason(waited.error) + ").");
      discard_session(session, transport);
      clear_active(state, request);
      active.reset();
    }
  }
}

void run_worker(const std::shared_ptr<DaemonHub::State> &state,
                const DaemonHubEnvironment &environment) noexcept {
  try {
    process_worker(state, environment);
  } catch (...) {
    std::lock_guard lock(state->mutex);
    fail_worker_locked(*state, "The LayerD link worker stopped unexpectedly.");
  }
}

std::shared_ptr<Request> submit_request(
    const std::shared_ptr<DaemonHub::State> &state,
    const DaemonHubEnvironment &environment, const Request::Kind kind,
    std::optional<CommittedProfile> committed = std::nullopt) {
  const auto timeout = std::max(environment.reply_timeout,
                                std::chrono::milliseconds::zero());
  auto request = std::make_shared<Request>(kind, std::move(committed),
                                           Clock::now() + timeout);
  {
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    std::unique_lock state_lock(state->mutex);
    if (state->stopping) {
      if (kind == Request::Kind::status)
        request->status_result.error = "The LayerD link was shut down.";
      else
        request->reload_result = {ReloadState::failed,
                                  "The LayerD link was shut down."};
      request->completed = true;
      return request;
    }
    if (!state->initialization_error.empty() || state->worker_failed) {
      const auto &reason = !state->initialization_error.empty()
                               ? state->initialization_error
                               : state->worker_error;
      if (kind == Request::Kind::status)
        request->status_result.error = reason;
      else
        request->reload_result = {ReloadState::failed, reason};
      request->completed = true;
      return request;
    }
    if (!state->worker.joinable()) {
      try {
        auto worker_state = state;
        auto worker_environment = environment;
        state->worker = std::thread(
            [worker_state = std::move(worker_state),
             worker_environment = std::move(worker_environment)] {
              run_worker(worker_state, worker_environment);
            });
      } catch (...) {
        if (kind == Request::Kind::status)
          request->status_result.error = "The LayerD link worker could not start.";
        else
          request->reload_result = {ReloadState::failed,
                                    "The LayerD link worker could not start."};
        request->completed = true;
        return request;
      }
    }
    state->queue.push_back(request);
  }

  if (SetEvent(state->wake_event) == FALSE) {
    std::lock_guard lock(state->mutex);
    if (!request->completed) {
      request->completed = true;
      if (kind == Request::Kind::status)
        request->status_result.error = "The LayerD link wake event failed.";
      else
        request->reload_result = {ReloadState::failed,
                                  "The LayerD link wake event failed."};
      request->ready.notify_all();
    }
    return request;
  }

  std::unique_lock lock(state->mutex);
  if (!request->ready.wait_until(lock, request->deadline + caller_backstop_grace,
                                 [&] { return request->completed; })) {
    request->completed = true;
    if (kind == Request::Kind::status) {
      request->status_result.running = request->request_sent.load();
      request->status_result.error = request->status_result.running
          ? "LayerD did not answer the status request before the deadline."
          : "LayerD did not authenticate the live pipe before the deadline.";
    } else {
      request->reload_result = {ReloadState::failed,
                                request->request_sent.load()
                                    ? "LayerD did not confirm the profile reload before the deadline."
                                    : "LayerD did not authenticate the live pipe before the deadline."};
    }
    request->ready.notify_all();
    lock.unlock();
    (void)SetEvent(state->wake_event);
  }
  return request;
}

} // namespace

DaemonHubEnvironment product_daemon_hub_environment() {
  return {.pipe_name = layerc::platform::live_edit_pipe_name,
          .expected_server_process_id = 0,
          .start_daemon = [] { return start_daemon(); },
          .signal_reload = [] { return layerc::platform::signal_profile_reload(); }};
}

DaemonHub::DaemonHub(DaemonHubEnvironment environment)
    : environment_(std::move(environment)), state_(std::make_shared<State>()) {}

DaemonHub::~DaemonHub() { shutdown(); }

DaemonStatus DaemonHub::status() {
  const auto request =
      submit_request(state_, environment_, Request::Kind::status);
  return request->status_result;
}

ReloadResult DaemonHub::reload(
    const std::optional<CommittedProfile> &committed) {
  const auto request = submit_request(state_, environment_, Request::Kind::reload,
                                      committed);
  return request->reload_result;
}

void DaemonHub::shutdown() noexcept {
  if (!state_) return;
  std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    if (state_->active) fail_shutdown_locked(state_->active);
    for (const auto &request : state_->queue) fail_shutdown_locked(request);
    state_->queue.clear();
  }
  if (state_->wake_event != nullptr) (void)SetEvent(state_->wake_event);
  if (state_->worker.joinable()) state_->worker.join();
}

} // namespace layerc::editor
