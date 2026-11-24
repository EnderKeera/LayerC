#include "platform/win32/daemon_hub_internal.hpp"

#include "daemon_process.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/diagnostics.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace layerc::editor {
namespace {
using daemon_hub_detail::Clock;
using daemon_hub_detail::Request;
using daemon_hub_detail::start_worker_locked;
using layerc::core::LiveMessage;
using layerc::core::LiveMessageType;
using layerc::core::ProfileRevision;

constexpr auto minimum_connect_wait = std::chrono::milliseconds(25);
// The worker owns every outcome, including the reload-signal fallback, and
// resolves each request by its deadline. A caller waits this much longer so it
// never races the worker for the result; it only gives up on a stuck worker.
constexpr auto caller_backstop_grace = std::chrono::milliseconds(500);

std::string win32_reason(DWORD error);
bool is_retry(const Request &request) noexcept {
  return request.kind == Request::Kind::retry_visual ||
         request.kind == Request::Kind::retry_automation;
}
bool is_trace(const Request &request) noexcept {
  return request.kind == Request::Kind::trace_subscribe ||
         request.kind == Request::Kind::trace_unsubscribe;
}

} // namespace

DaemonHub::State::State() {
  wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (wake_event == nullptr) {
    const DWORD error = GetLastError();
    initialization_error = "The LayerD link wake event could not be created (" +
                           win32_reason(error) + ").";
  }
}
DaemonHub::State::~State() {
  if (wake_event != nullptr) CloseHandle(wake_event);
}

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

void finish_retry(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request,
                  DaemonCommandResult result) {
  std::lock_guard lock(state->mutex);
  if (request->completed) return;
  request->retry_result = std::move(result);
  request->completed = true;
  request->ready.notify_all();
}

void finish_trace(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request,
                  std::optional<std::string> failure) {
  std::lock_guard lock(state->mutex);
  if (request->completed) return;
  request->completed = true;
  if (request->kind == Request::Kind::trace_unsubscribe) {
    if (!failure) state->trace.unsubscribe_succeeded();
  } else if (failure) {
    state->trace.attempt_failed(request->trace_id, std::move(*failure));
  } else {
    state->trace.attempt_succeeded(request->trace_id);
  }
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
  if (is_trace(*request)) {
    finish_trace(state, request, std::move(reason));
    return;
  }
  if (request->kind == Request::Kind::status) {
    finish_status(state, request, disconnected_status(std::move(reason)));
    return;
  }
  if (is_retry(*request)) {
    finish_retry(state, request, {DaemonCommandState::daemon_unavailable, std::move(reason)});
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
  if (is_trace(*request)) {
    finish_trace(state, request, std::move(reason));
    return;
  }
  if (request->kind == Request::Kind::status) {
    finish_status(state, request, disconnected_status(std::move(reason)));
    return;
  }
  if (is_retry(*request))
    finish_retry(state, request, {DaemonCommandState::daemon_unavailable, std::move(reason)});
  else
    finish_reload(state, request, {ReloadState::failed, std::move(reason)});
}

void fail_timeout(const std::shared_ptr<DaemonHub::State> &state,
                  const std::shared_ptr<Request> &request) {
  if (is_trace(*request)) {
    finish_trace(state, request,
                 "LayerD did not confirm the trace subscription before the deadline.");
    return;
  }
  if (request->kind == Request::Kind::status) {
    DaemonStatus status;
    status.running = request->request_sent.load();
    status.error = status.running
                       ? "LayerD did not answer the status request before the deadline."
                       : "LayerD did not authenticate the live pipe before the deadline.";
    finish_status(state, request, std::move(status));
    return;
  }
  if (is_retry(*request))
    finish_retry(state, request, {DaemonCommandState::daemon_unavailable,
                 "LayerD did not answer the retry request before the deadline."});
  else
    finish_reload(state, request,
                  {ReloadState::failed,
                   "LayerD did not confirm the profile reload before the deadline."});
}

void set_unavailable_result(Request &request, std::string reason) {
  if (request.kind == Request::Kind::status)
    request.status_result.error = std::move(reason);
  else if (is_retry(request))
    request.retry_result = {DaemonCommandState::daemon_unavailable, std::move(reason)};
  else if (!is_trace(request))
    request.reload_result = {ReloadState::failed, std::move(reason)};
}

void fail_shutdown_locked(const std::shared_ptr<Request> &request) {
  if (request->completed) return;
  set_unavailable_result(*request, "The LayerD link was shut down.");
  request->completed = true;
  request->ready.notify_all();
}

// Ends the worker: every waiting caller gets the reason and later requests
// are refused with it instead of queueing behind a thread that is gone.
void fail_worker_locked(DaemonHub::State &state, std::string reason) {
  state.worker_failed = true;
  state.trace.fail(reason);
  state.worker_error = std::move(reason);
  if (state.active) fail_shutdown_locked(state.active);
  for (const auto &request : state.queue) fail_shutdown_locked(request);
  state.queue.clear();
  state.active.reset();
}

void handle_absent(const std::shared_ptr<DaemonHub::State> &state,
                   const std::shared_ptr<Request> &request,
                   const DaemonHubEnvironment &environment) {
  if (is_trace(*request)) {
    finish_trace(state, request,
                 "LayerD is not running; start LayerD and try again.");
    return;
  }
  if (request->kind == Request::Kind::status) {
    DaemonStatus status;
    status.socket_missing = true;
    finish_status(state, request, std::move(status));
    return;
  }
  if (is_retry(*request)) {
    finish_retry(state, request, {DaemonCommandState::daemon_unavailable, "LayerD is not running."});
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
  if (is_trace(*request)) {
    if (message.type == LiveMessageType::acknowledgement) {
      finish_trace(state, request, std::nullopt);
      return true;
    }
    if (message.type == LiveMessageType::error) {
      finish_trace(state, request,
                   message.text.empty()
                       ? "LayerD refused the trace subscription."
                       : message.text);
      return true;
    }
    return false;
  }
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
  if (is_retry(*request)) {
    if (message.type != LiveMessageType::acknowledgement &&
        message.type != LiveMessageType::error) return false;
    finish_retry(state, request,
                 {message.type == LiveMessageType::acknowledgement
                      ? DaemonCommandState::acknowledged : DaemonCommandState::refused,
                  message.text});
    return true;
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

// Routes one valid, authenticated poll in arrival order: pushed trace
// packets reach the relay, and the active request's reply completes it.
std::optional<LiveMessageType>
route_forwarded(const std::shared_ptr<DaemonHub::State> &state,
                const LiveEditStep &step, const ActiveDemand *active) {
  std::optional<LiveMessageType> reply;
  for (const auto &message : step.forwarded) {
    if (message.type == LiveMessageType::automation_trace_event) {
      std::lock_guard lock(state->mutex);
      state->trace.append_packet(message.text);
      continue;
    }
    if (!reply && active != nullptr && active->request->request_sent.load() &&
        handle_reply(state, active->request, active->reply_revision, message))
      reply = message.type;
  }
  return reply;
}

// A closed connection also drops LayerD's trace subscription.
void discard_session(const std::shared_ptr<DaemonHub::State> &state,
                     std::unique_ptr<LiveEditSession> &session,
                     PipeLiveEditTransport *&transport) {
  session.reset();
  transport = nullptr;
  std::lock_guard lock(state->mutex);
  state->trace.connection_lost();
}

bool skip_unsent_trace(const std::shared_ptr<DaemonHub::State> &state,
                       const Request &request, const LiveEditSession *session) {
  if (!is_trace(request) || request.request_sent.load()) return false;
  // A closed connection already dropped LayerD's trace subscription.
  if (request.kind == Request::Kind::trace_unsubscribe)
    return session == nullptr || !session->connected();
  std::lock_guard lock(state->mutex);
  return !state->trace.current(request.trace_id);
}

HubEditDriver make_edit_driver(const std::shared_ptr<DaemonHub::State> &state) {
  return HubEditDriver([state](const std::uint64_t id, OnWindowEditEvent event) {
    std::function<void()> listener;
    {
      std::lock_guard lock(state->mutex);
      if (id != state->edit_current) return;
      state->edit_events.push_back(std::move(event));
      listener = state->edit_listener;
    }
    if (listener) listener();
  });
}

void process_worker(const std::shared_ptr<DaemonHub::State> &state,
                    const DaemonHubEnvironment &environment) {
  std::unique_ptr<LiveEditSession> session;
  PipeLiveEditTransport *transport{};
  std::optional<ActiveDemand> active;
  std::optional<DirectDragRelease> carried;
  auto edit = make_edit_driver(state);
  // A closed connection ends the edit session with it.
  const auto discard = [&](std::string reason = "LayerD's live pipe closed.") {
    if (session) daemon_hub_detail::carry_session_release(*state, *session, carried);
    edit.connection_discarded(std::move(reason));
    discard_session(state, session, transport);
  };
  // The edit session's connection; LayerD is launched when its pipe is absent.
  const HubEditDriver::OpenSession open =
      [&](bool &started, std::string &reason) -> LiveEditSession * {
    if (session && session->connected()) return session.get();
    if (environment.pipe_name.empty()) {
      reason = "LayerD live pipe name is empty.";
      return nullptr;
    }
    if (WaitNamedPipeW(environment.pipe_name.c_str(), 1) == FALSE) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND) {
        reason = pipe_unreachable_reason(error);
        return nullptr;
      }
      if (!call_service(environment.start_daemon)) {
        reason = "LayerD is not running and could not be started.";
        return nullptr;
      }
      started = true;
    }
    if (!session) daemon_hub_detail::open_connection(environment, session, transport, carried);
    return session.get();
  };
  bool poll_now{};

  for (;;) {
    std::deque<HubEditCommand> commands;
    std::optional<std::pair<std::uint64_t, bool>> unqueued_end;
    std::uint64_t swapped_seq{};
    {
      std::lock_guard lock(state->mutex);
      if (state->stopping) break;
      commands.swap(state->edit_commands);
      swapped_seq = state->drag_enable_seq;
      unqueued_end = std::exchange(state->unqueued_end, {});
      if (!active && !state->queue.empty()) {
        auto request = std::move(state->queue.front());
        state->queue.pop_front();
        // No caller waits on a trace request, so its deadline bounds only
        // its own exchange, not the queue ahead of it.
        if (is_trace(*request))
          request->deadline = Clock::now() +
              std::max(environment.reply_timeout, std::chrono::milliseconds::zero());
        state->active = request;
        active.emplace(ActiveDemand{std::move(request)});
      }
    }
    for (auto &command : commands) edit.apply(std::move(command), session.get(), open);
    if (unqueued_end)
      edit.apply({.kind = HubEditCommand::Kind::end, .id = unqueued_end->first,
                  .flag = unqueued_end->second}, session.get(), open);
    const bool reconnecting = !session && carried.has_value();
    if (reconnecting)
      daemon_hub_detail::reconnect_for_release(environment, session, transport, carried);
    const auto publish_release = [&] {
      daemon_hub_detail::publish_drag_release(*state, swapped_seq, session.get(), carried.has_value());
    };
    publish_release();
    if (!commands.empty() || unqueued_end || (reconnecting && session)) poll_now = true;

    if (!active) {
      if (session && (session->connected() || edit.live() || session->release_pending())) {
        if (!poll_now) {
          const auto waited = wait_for_work(state, transport, edit.wait_timeout(*session));
          if (waited.kind == WaitResult::Kind::wake) continue;
          {
            std::lock_guard lock(state->mutex);
            if (state->stopping || !state->edit_commands.empty() || state->unqueued_end) continue;
          }
          if (waited.kind == WaitResult::Kind::failed) {
            discard("LayerD live-pipe wait failed (" + win32_reason(waited.error) + ").");
            continue;
          }
        }
        poll_now = false;
        const auto step = session->poll({});
        publish_release();
        edit.apply_step(step, *session);
        if (step.authentication_rejected) {
          discard("LayerD refused live-pipe package authentication.");
        } else if (!step.protocol_errors.empty()) {
          discard("LayerD sent an invalid live-edit protocol message.");
        } else if (!session->connected() && !edit.live() && !session->release_pending()) {
          discard();
        } else if (session->authenticated()) {
          (void)route_forwarded(state, step, nullptr);
        }
        continue;
      }
      poll_now = false;
      if (session) discard();
      if (wait_for_work(state, nullptr, carried ? HubEditDriver::reconnect_ms : INFINITE).kind ==
          WaitResult::Kind::failed) {
        std::lock_guard lock(state->mutex);
        fail_worker_locked(*state, "The LayerD link worker could not wait for requests.");
        return;
      }
      continue;
    }

    const auto request = active->request;
    if (request_completed(state, request) ||
        skip_unsent_trace(state, *request, session.get())) {
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (Clock::now() >= request->deadline) {
      // The remote subscription state is unknown without a reply. Closing
      // the pipe guarantees an abandoned recording stops.
      if (is_trace(*request)) discard();
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
      // A session waiting for LayerD keeps reconnecting on its own.
      if (session && !session->connected() && !edit.live() && !session->release_pending())
        discard();
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
        daemon_hub_detail::open_connection(environment, session, transport, carried);
      }
      active->presence_probed = true;
    }

    poll_now = false;
    const auto step = session->poll({});
    publish_release();
    edit.apply_step(step, *session);
    if (step.authentication_rejected) {
      fail_unreachable(state, request, environment,
                       "LayerD refused live-pipe package authentication.");
      discard();
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
      discard();
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (!session->authenticated()) {
      if (!session->connected() && transport != nullptr &&
          !transport->last_error().empty()) {
        fail_unreachable(state, request, environment,
                         "LayerD live-pipe authentication failed.");
        discard();
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
        discard();
        clear_active(state, request);
        active.reset();
      }
      continue;
    }

    const auto reply = route_forwarded(state, step, &*active);
    if (!request->request_sent.load()) {
      bool sent{};
      if (request->kind == Request::Kind::status) {
        sent = session->send_authenticated(
            LiveMessage{.type = LiveMessageType::diagnostics_request});
      } else if (is_trace(*request)) {
        sent = session->send_authenticated(LiveMessage{
            .type = request->kind == Request::Kind::trace_subscribe
                        ? LiveMessageType::automation_trace_subscribe
                         : LiveMessageType::automation_trace_unsubscribe});
      } else if (is_retry(*request)) {
        sent = session->send_authenticated(std::move(request->retry_message));
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
        discard();
        clear_active(state, request);
        active.reset();
        continue;
      }
      active->reply_revision = session->revision();
      request->request_sent.store(true);
      continue;
    }

    if (reply || request_completed(state, request)) {
      if (request->kind == Request::Kind::trace_unsubscribe &&
          reply == LiveMessageType::error)
        discard();
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (!session->connected()) {
      fail_closed(state, request,
                  "LayerD closed the live pipe before replying.");
      discard();
      clear_active(state, request);
      active.reset();
      continue;
    }
    if (poll_yielded(step)) continue;

    // An edit session's heartbeat keeps its cadence beneath a request.
    const auto waited = wait_for_work(
        state, transport,
        std::min(remaining_timeout(request->deadline),
                 edit.wait_timeout(*session)));
    if (waited.kind == WaitResult::Kind::failed) {
      fail_closed(state, request,
                  "LayerD live-pipe wait failed (" +
                      win32_reason(waited.error) + ").");
      discard();
      clear_active(state, request);
      active.reset();
    }
  }
  edit.shutdown(session.get());
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
    std::optional<CommittedProfile> committed = std::nullopt,
    LiveMessage retry_message = {}) {
  const auto timeout = std::max(environment.reply_timeout,
                                std::chrono::milliseconds::zero());
  auto request = std::make_shared<Request>(kind, std::move(committed),
                                           Clock::now() + timeout);
  request->retry_message = std::move(retry_message);
  {
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    std::unique_lock state_lock(state->mutex);
    if (state->stopping) {
      set_unavailable_result(*request, "The LayerD link was shut down.");
      request->completed = true;
      return request;
    }
    if (!state->initialization_error.empty() || state->worker_failed) {
      const auto &reason = !state->initialization_error.empty()
                               ? state->initialization_error
                               : state->worker_error;
      set_unavailable_result(*request, reason);
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
        set_unavailable_result(*request, "The LayerD link worker could not start.");
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
      set_unavailable_result(*request, "The LayerD link wake event failed.");
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
      set_unavailable_result(*request, !request->request_sent.load()
          ? "LayerD did not authenticate the live pipe before the deadline."
          : is_retry(*request)
              ? "LayerD did not answer the retry request before the deadline."
              : "LayerD did not confirm the profile reload before the deadline.");
    }
    request->ready.notify_all();
    lock.unlock();
    (void)SetEvent(state->wake_event);
  }
  return request;
}

} // namespace

namespace daemon_hub_detail {

std::optional<std::string> start_worker_locked(
    const std::shared_ptr<DaemonHub::State> &state,
    const DaemonHubEnvironment &environment) {
  if (state->stopping) return "The LayerD link was shut down.";
  if (!state->initialization_error.empty()) return state->initialization_error;
  if (state->worker_failed) return state->worker_error;
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
      return "The LayerD link worker could not start.";
    }
  }
  return std::nullopt;
}

} // namespace daemon_hub_detail

namespace {

std::optional<std::string> enqueue_trace_locked(
    const std::shared_ptr<DaemonHub::State> &state,
    const DaemonHubEnvironment &environment, const Request::Kind kind,
    const std::uint64_t id) {
  if (auto failure = start_worker_locked(state, environment)) return failure;
  auto request = std::make_shared<Request>(kind, std::nullopt, Clock::now());
  request->trace_id = id;
  state->queue.push_back(request);
  if (SetEvent(state->wake_event) == FALSE) {
    std::erase(state->queue, request);
    return "The LayerD link wake event failed.";
  }
  return std::nullopt;
}

} // namespace

DaemonHubEnvironment product_daemon_hub_environment() {
  return {.pipe_name = layerc::platform::live_edit_pipe_name,
          .expected_server_process_id = 0,
          .start_daemon = [] { return start_daemon(); },
          .signal_reload = [] { return layerc::platform::signal_profile_reload(); },
          .product = true};
}

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

bool daemon_retry_supported() noexcept { return true; }

DaemonCommandResult request_visual_retry() {
  return product_daemon_hub().retry_visual();
}

DaemonCommandResult request_automation_retry(const std::filesystem::path &profile,
                                             const std::string_view automation_id) {
  return product_daemon_hub().retry_automation(profile, automation_id);
}

DaemonCommandResult DaemonHub::retry_visual() {
  return submit_request(state_, environment_, Request::Kind::retry_visual,
                        std::nullopt, {.type = LiveMessageType::retry_visual})->retry_result;
}

DaemonCommandResult DaemonHub::retry_automation(
    const std::filesystem::path &profile, const std::string_view automation_id) {
  auto key = layerc::platform::canonical_profile_key(profile);
  if (key.empty())
    return {DaemonCommandState::refused, "The profile path could not be canonicalized."};
  return submit_request(state_, environment_, Request::Kind::retry_automation, std::nullopt,
                        {.type = LiveMessageType::retry_automation,
                         .profile_key = std::move(key), .text = std::string(automation_id)})
      ->retry_result;
}

std::uint64_t DaemonHub::subscribe_trace() {
  std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
  std::lock_guard lock(state_->mutex);
  const auto id = state_->trace.subscribe();
  if (auto failure = enqueue_trace_locked(state_, environment_,
                                          Request::Kind::trace_subscribe, id))
    state_->trace.fail(std::move(*failure));
  return id;
}

void DaemonHub::poll_trace(const std::uint64_t subscription) {
  std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
  std::lock_guard lock(state_->mutex);
  if (!state_->trace.begin_attempt(subscription)) return;
  if (auto failure = enqueue_trace_locked(state_, environment_,
                                          Request::Kind::trace_subscribe,
                                          subscription))
    state_->trace.fail(std::move(*failure));
}

std::optional<AutomationTraceUpdate>
DaemonHub::take_trace(const std::uint64_t subscription) {
  std::lock_guard lock(state_->mutex);
  return state_->trace.take_update(subscription);
}

void DaemonHub::unsubscribe_trace(const std::uint64_t subscription) noexcept {
  try {
    std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
    std::lock_guard lock(state_->mutex);
    if (state_->trace.unsubscribe(subscription))
      (void)enqueue_trace_locked(state_, environment_,
                                 Request::Kind::trace_unsubscribe, 0);
  } catch (...) {
    // Closing the pipe at shutdown also ends LayerD's subscription.
  }
}

void DaemonHub::shutdown() noexcept {
  if (!state_) return;
  std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    state_->trace.fail("The LayerD link was shut down.");
    if (state_->active) fail_shutdown_locked(state_->active);
    for (const auto &request : state_->queue) fail_shutdown_locked(request);
    state_->queue.clear();
  }
  if (state_->wake_event != nullptr) (void)SetEvent(state_->wake_event);
  if (state_->worker.joinable()) state_->worker.join();
}

} // namespace layerc::editor
