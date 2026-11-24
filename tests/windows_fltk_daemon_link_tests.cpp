#include "platform/win32/daemon_hub_internal.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/diagnostics.hpp"
#include "layerc/runtime/runtime_services.hpp"
#include "test_support.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::editor {
struct DaemonHubTestAccess {
  static void before_release_publish(DaemonHub &hub, std::function<void()> callback) {
    std::lock_guard lock(hub.state_->mutex);
    hub.state_->before_release_publish_for_test = std::move(callback);
  }
  static void on_release_carried(DaemonHub &hub,
                                 std::function<void(DirectDragRelease &)> callback) {
    std::lock_guard lock(hub.state_->mutex);
    hub.state_->on_release_carried_for_test = std::move(callback);
  }
  static void fail_edit_wake(DaemonHub &hub) {
    std::lock_guard lock(hub.state_->mutex);
    hub.state_->fail_edit_wake_for_test = true;
  }
};
} // namespace layerc::editor

namespace {
using namespace std::chrono_literals;
namespace core = layerc::core;
namespace editor = layerc::editor;
namespace platform = layerc::platform;
namespace runtime = layerc::runtime;

std::wstring private_pipe_name() {
  static std::atomic<std::uint64_t> sequence{};
  std::wstring name = L"\\\\.\\pipe\\LayerC.P2B2.Tests.";
  name += std::to_wstring(GetCurrentProcessId());
  name += L".";
  name += std::to_wstring(sequence.fetch_add(1));
  return name;
}

std::optional<std::size_t> process_thread_count() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  THREADENTRY32 entry{};
  entry.dwSize = static_cast<DWORD>(sizeof(entry));
  std::size_t count{};
  if (Thread32First(snapshot, &entry) != FALSE) {
    do {
      if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++count;
    } while (Thread32Next(snapshot, &entry) != FALSE);
  } else {
    CloseHandle(snapshot);
    return std::nullopt;
  }
  CloseHandle(snapshot);
  return count;
}

std::string test_diagnostics_json() {
  runtime::RuntimeDiagnostics diagnostics;
  diagnostics.package_version = "test-build";
  diagnostics.sessions.resize(2);
  runtime::RuntimeServiceStats overlay;
  overlay.capability = runtime::Capability::overlay;
  overlay.state = runtime::ServiceState::active;
  diagnostics.services.push_back(overlay);
  return runtime::serialize_runtime_diagnostics_json(diagnostics);
}

enum class ReplyMode { acknowledge, error, silent, silent_trace_unsubscribe };

class FakeLayerD final {
public:
  explicit FakeLayerD(const ReplyMode mode,
                      const DWORD expected_client_pid = GetCurrentProcessId())
      : pipe_name_(private_pipe_name()), mode_(mode),
        expected_client_pid_(expected_client_pid),
        stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        work_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
        diagnostics_json_(test_diagnostics_json()) {}

  ~FakeLayerD() {
    stop();
    if (work_event_ != nullptr) CloseHandle(work_event_);
    if (stop_event_ != nullptr) CloseHandle(stop_event_);
  }

  FakeLayerD(const FakeLayerD &) = delete;
  FakeLayerD &operator=(const FakeLayerD &) = delete;

  void stop() {
    if (stop_event_ != nullptr) (void)SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
  }

  [[nodiscard]] bool start() {
    if (stop_event_ == nullptr || work_event_ == nullptr || !ResetEvent(stop_event_)) return false;
    ready_ = false;
    thread_ = std::thread([this] { serve(); });
    std::unique_lock lock(mutex_);
    return ready_cv_.wait_for(lock, 2s, [this] { return ready_; }) && pipe_ready_;
  }

  [[nodiscard]] const std::wstring &pipe_name() const noexcept {
    return pipe_name_;
  }

  [[nodiscard]] bool wait_for_diagnostics(const std::size_t count,
                                          const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return request_cv_.wait_for(lock, timeout,
                                [this, count] { return diagnostics_count_ >= count; });
  }

  [[nodiscard]] std::size_t handshakes() {
    std::lock_guard lock(mutex_);
    return handshake_count_;
  }

  void release_proof_capability(bool capable) { release_proof_ = capable; }

  [[nodiscard]] bool pause_reads(bool pause) {
    std::unique_lock lock(mutex_);
    pause_reads_requested_ = pause;
    return SetEvent(work_event_) != FALSE &&
           request_cv_.wait_for(lock, 2s, [&] { return reads_paused_ == pause; });
  }

  void hold_off_ack(bool hold) {
    hold_off_ack_ = hold;
    (void)SetEvent(work_event_);
  }

  [[nodiscard]] bool wait_for_trace_subscribes(
      const std::size_t count, const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return request_cv_.wait_for(lock, timeout,
                                [this, count] {
                                  return trace_subscribe_count_ >= count;
                                });
  }

  [[nodiscard]] bool wait_for_trace_unsubscribes(
      const std::size_t count, const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return request_cv_.wait_for(lock, timeout,
                                [this, count] {
                                  return trace_unsubscribe_count_ >= count;
                                });
  }

  [[nodiscard]] std::size_t trace_subscribe_count() {
    std::lock_guard lock(mutex_);
    return trace_subscribe_count_;
  }

  [[nodiscard]] std::size_t trace_unsubscribe_count() {
    std::lock_guard lock(mutex_);
    return trace_unsubscribe_count_;
  }

  [[nodiscard]] bool push_trace(std::string packet) {
    {
      std::lock_guard lock(mutex_);
      pending_trace_packets_.push_back(std::move(packet));
    }
    return work_event_ != nullptr && SetEvent(work_event_) != FALSE;
  }

  [[nodiscard]] bool wait_for_disconnect(const std::chrono::milliseconds timeout,
                                         const std::size_t count = 1) {
    std::unique_lock lock(mutex_);
    return request_cv_.wait_for(lock, timeout, [&] { return disconnects_ >= count; });
  }

  [[nodiscard]] std::size_t disconnects() {
    std::lock_guard lock(mutex_);
    return disconnects_;
  }

  [[nodiscard]] bool disconnect_client() {
    {
      std::lock_guard lock(mutex_);
      disconnect_requested_ = true;
    }
    return work_event_ != nullptr && SetEvent(work_event_) != FALSE;
  }

  struct EditPacket {
    core::LiveMessage message;
    std::chrono::steady_clock::time_point received;
  };

  [[nodiscard]] std::vector<EditPacket> edit_packets() {
    std::lock_guard lock(mutex_);
    return edit_packets_;
  }

  [[nodiscard]] std::vector<core::LiveMessage> retry_packets() {
    std::lock_guard lock(mutex_);
    return retry_packets_;
  }
  [[nodiscard]] bool wait_for_edit_packets(core::LiveMessageType type, std::size_t count,
                                           std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return request_cv_.wait_for(lock, timeout, [&] {
      std::size_t seen{};
      for (const auto &packet : edit_packets_)
        if (packet.message.type == type && ++seen == count) return true;
      return false;
    });
  }

private:
  void reply(platform::NamedPipeEditorDaemonChannel &channel,
             const core::LiveMessage &request, const core::LiveMessageType type,
             std::string text = {}) {
    core::LiveMessage message;
    message.type = type;
    message.revision = request.revision;
    message.edit_session = request.edit_session;
    message.text = std::move(text);
    (void)channel.send(message);
  }

  void handle(platform::NamedPipeEditorDaemonChannel &channel,
              const core::LiveMessage &message) {
    if (message.type == core::LiveMessageType::handshake) {
      {
        std::lock_guard lock(mutex_);
        ++handshake_count_;
      }
      reply(channel, message, core::LiveMessageType::acknowledgement,
            core::package_identity("LayerD") + ";" +
                (release_proof_ ? std::string(core::live_release_proof_capability) : "test"));
      return;
    }
    if (message.type == core::LiveMessageType::diagnostics_request) {
      {
        std::lock_guard lock(mutex_);
        ++diagnostics_count_;
      }
      request_cv_.notify_all();
      if (mode_ != ReplyMode::silent)
        reply(channel, message, core::LiveMessageType::diagnostics_reply,
              diagnostics_json_);
      return;
    }
    if (message.type == core::LiveMessageType::automation_trace_subscribe) {
      {
        std::lock_guard lock(mutex_);
        ++trace_subscribe_count_;
      }
      request_cv_.notify_all();
      if (mode_ == ReplyMode::error) {
        reply(channel, message, core::LiveMessageType::error,
              "test trace subscribe rejected");
      } else if (mode_ != ReplyMode::silent) {
        reply(channel, message, core::LiveMessageType::acknowledgement,
              "trace subscribed");
      }
      return;
    }
    if (message.type == core::LiveMessageType::automation_trace_unsubscribe) {
      {
        std::lock_guard lock(mutex_);
        ++trace_unsubscribe_count_;
      }
      request_cv_.notify_all();
      if (mode_ != ReplyMode::silent_trace_unsubscribe)
        reply(channel, message, core::LiveMessageType::acknowledgement,
              "trace unsubscribed");
      return;
    }
    if (message.type == core::LiveMessageType::retry_visual ||
        message.type == core::LiveMessageType::retry_automation) {
      {
        std::lock_guard lock(mutex_);
        retry_packets_.push_back(message);
      }
      if (mode_ == ReplyMode::error)
        reply(channel, message, core::LiveMessageType::error, "test retry refused");
      else if (mode_ != ReplyMode::silent)
        reply(channel, message, core::LiveMessageType::acknowledgement,
              message.type == core::LiveMessageType::retry_visual
                  ? "visual runtime retry requested" : "automation retry requested");
      return;
    }
    if (message.edit_session != 0) {
      {
        std::lock_guard lock(mutex_);
        edit_packets_.push_back({message, std::chrono::steady_clock::now()});
      }
      request_cv_.notify_all();
      // Like LayerD, answer an applied Direct drag off; a silent LayerD never does.
      if (message.type == core::LiveMessageType::set_direct_drag && !message.direct_drag &&
          mode_ != ReplyMode::silent) {
        if (hold_off_ack_) held_off_ = message;
        else reply(channel, message, core::LiveMessageType::acknowledgement,
                   std::string(core::live_direct_drag_off_ack));
      }
      if (message.type == core::LiveMessageType::begin_edit) {
        reply(channel, message, core::LiveMessageType::acknowledgement);
        (void)channel.send({.type = core::LiveMessageType::anchor_changed,
                            .revision = message.revision + 1,
                            .edit_session = message.edit_session, .panel_id = message.panel_id,
                            .anchor_x = .2, .anchor_y = .3, .final = true});
      }
      return;
    }
    if (message.type != core::LiveMessageType::committed_profile) return;
    if (mode_ == ReplyMode::silent) return;
    if (mode_ == ReplyMode::error) {
      reply(channel, message, core::LiveMessageType::error,
            "test reload rejected");
    } else {
      reply(channel, message, core::LiveMessageType::acknowledgement,
            "profile applied");
    }
  }

  void serve() {
    auto channel = std::make_unique<platform::NamedPipeEditorDaemonChannel>(
        platform::NamedPipeEditorDaemonChannel::Role::server, pipe_name_,
        expected_client_pid_);
    const bool pipe_ready = WaitNamedPipeW(pipe_name_.c_str(), 1000) != FALSE;
    {
      std::lock_guard lock(mutex_);
      pipe_ready_ = pipe_ready;
      ready_ = true;
    }
    ready_cv_.notify_all();
    if (!pipe_ready) return;

    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      std::deque<std::string> trace_packets;
      bool disconnect{};
      bool reads_paused{};
      {
        std::lock_guard lock(mutex_);
        reads_paused = reads_paused_ = pause_reads_requested_;
        if (!reads_paused) {
          disconnect = std::exchange(disconnect_requested_, false);
          trace_packets.swap(pending_trace_packets_);
        }
      }
      request_cv_.notify_all();
      if (reads_paused) {
        const HANDLE handles[]{stop_event_, work_event_};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        continue;
      }
      const bool was_connected = channel->connected();
      if (disconnect) channel->disconnect_peer();
      if (!hold_off_ack_ && held_off_) {
        reply(*channel, *held_off_, core::LiveMessageType::acknowledgement,
              std::string(core::live_direct_drag_off_ack));
        held_off_.reset();
      }
      for (auto &packet : trace_packets) {
        core::LiveMessage message;
        message.type = core::LiveMessageType::automation_trace_event;
        message.text = std::move(packet);
        (void)channel->send(message);
      }
      for (const auto &result : channel->poll()) {
        if (const auto *message = std::get_if<core::LiveMessage>(&result))
          handle(*channel, *message);
      }
      // One poll can drop the peer and accept the next client at once, so
      // connected() alone would miss that disconnect.
      const bool changed = channel->take_connection_changed();
      if (was_connected && (changed || !channel->connected())) {
        held_off_.reset();
        {
          std::lock_guard lock(mutex_);
          ++disconnects_;
        }
        request_cv_.notify_all();
      }
      if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
      std::array<HANDLE, 5> handles{};
      handles[0] = stop_event_;
      handles[1] = work_event_;
      const auto pipe_count = channel->wait_handles(
          std::span<HANDLE>(handles).subspan(2));
      const DWORD count = static_cast<DWORD>(pipe_count + 2U);
      const DWORD waited =
          WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);
      if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
    }
  }

  std::wstring pipe_name_;
  std::vector<core::LiveMessage> retry_packets_;
  ReplyMode mode_;
  DWORD expected_client_pid_{};
  HANDLE stop_event_{};
  HANDLE work_event_{};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::condition_variable request_cv_;
  bool ready_{};
  bool pipe_ready_{};
  bool pause_reads_requested_{};
  bool reads_paused_{};
  std::atomic<bool> release_proof_{true};
  std::atomic<bool> hold_off_ack_{};
  std::optional<core::LiveMessage> held_off_;
  std::size_t diagnostics_count_{};
  std::size_t handshake_count_{};
  std::size_t trace_subscribe_count_{};
  std::size_t trace_unsubscribe_count_{};
  bool disconnect_requested_{};
  std::size_t disconnects_{};
  std::deque<std::string> pending_trace_packets_;
  std::vector<EditPacket> edit_packets_;
  std::string diagnostics_json_;
};

editor::DaemonHubEnvironment make_environment(
    std::wstring pipe_name, std::atomic<unsigned> &starts,
    std::atomic<unsigned> &signals, const std::chrono::milliseconds timeout = 2s) {
  editor::DaemonHubEnvironment environment;
  environment.pipe_name = std::move(pipe_name);
  environment.expected_server_process_id = GetCurrentProcessId();
  environment.start_daemon = [&starts] {
    ++starts;
    return true;
  };
  environment.signal_reload = [&signals] {
    ++signals;
    return true;
  };
  environment.reply_timeout = timeout;
  return environment;
}

std::optional<editor::CommittedProfile>
make_committed_profile(layerc::test::TempDirectory &temporary) {
  const auto path = temporary.path() / "profile.json";
  constexpr std::string_view json = "{}";
  if (!layerc::test::write_file(path, json)) return std::nullopt;
  return editor::CommittedProfile{path, std::string(json)};
}

core::AutomationEvent make_trace_event(const std::uint64_t id) {
  core::AutomationEvent event;
  event.kind = core::AutomationEventKind::session_started;
  event.timestamp = core::TimePoint{} + core::Milliseconds{
      static_cast<std::int64_t>(id)};
  event.source_id = "hub-event-" + std::to_string(id);
  event.origin_token = id;
  event.fields.emplace("id", core::AutomationValue{
                                  static_cast<std::int64_t>(id)});
  return event;
}

std::string trace_packet(const std::vector<core::AutomationEvent> &events) {
  std::vector<core::AutomationTraceEntry> entries;
  entries.reserve(events.size());
  for (std::size_t index{}; index < events.size(); ++index)
    entries.push_back(
        core::make_automation_root_trace_entry(events[index], index + 1U));
  return core::export_layerc_trace(entries);
}

std::optional<editor::AutomationTraceUpdate> wait_for_trace_update(
    editor::DaemonHub &hub, const std::uint64_t id,
    const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto update = hub.take_trace(id)) return update;
    std::this_thread::sleep_for(20ms);
  }
  return hub.take_trace(id);
}

void lazy_worker_and_absent_status() {
  const auto before = process_thread_count();
  if (!layerc::test::expect(before.has_value(),
                           "the process thread list is available"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(private_pipe_name(), starts, signals));
  const auto after_construct = process_thread_count();
  layerc::test::expect(after_construct == before,
                       "constructing the hub starts no worker thread");

  const auto status = hub.status();
  layerc::test::expect(status.socket_missing && !status.running,
                       "an absent private pipe reports a missing daemon");
  hub.shutdown();
  layerc::test::expect(process_thread_count() == before,
                       "shutdown joins the lazily started worker");
}

void absent_reload_starts_once_without_claiming_success() {
  layerc::test::TempDirectory temporary("layerc-p2b1-absent");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(private_pipe_name(), starts, signals));
  const auto result = hub.reload(profile);
  layerc::test::expect(result.state == editor::ReloadState::daemon_unavailable &&
                           starts == 1 && signals == 0 && !result.error.empty(),
                       "an absent daemon is started once but reload remains unconfirmed");
}

void acknowledgement_confirms_committed_reload() {
  layerc::test::TempDirectory temporary("layerc-p2b1-ack");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto result = hub.reload(profile);
  layerc::test::expect(result.state == editor::ReloadState::acknowledged &&
                           starts == 0 && signals == 0,
                       "only the matching LayerD acknowledgement confirms reload");
}

void error_reply_fails_with_layerd_text() {
  layerc::test::TempDirectory temporary("layerc-p2b1-error");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  FakeLayerD server(ReplyMode::error);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto result = hub.reload(profile);
  layerc::test::expect(result.state == editor::ReloadState::failed &&
                           result.error == "test reload rejected",
                       "an error reply is returned as a failed reload with its text");
}

void absent_reply_times_out_within_deadline() {
  layerc::test::TempDirectory temporary("layerc-p2b1-timeout");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  FakeLayerD server(ReplyMode::silent);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals, 300ms));
  const auto began = std::chrono::steady_clock::now();
  const auto result = hub.reload(profile);
  const auto elapsed = std::chrono::steady_clock::now() - began;
  layerc::test::expect(result.state == editor::ReloadState::failed &&
                           result.error.find("deadline") != std::string::npos &&
                           elapsed < 2s,
                       "a missing reply fails within the monotonic deadline");
}

void rejected_client_fails_and_signals_without_launching() {
  const DWORD process_id = GetCurrentProcessId();
  const DWORD refused_client = process_id == 1 ? 2 : process_id - 1;
  FakeLayerD server(ReplyMode::acknowledge, refused_client);
  if (!layerc::test::expect(server.start(), "the private refusal pipe is ready"))
    return;
  layerc::test::TempDirectory temporary("layerc-p2b1-refused");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals, 700ms));
  const auto result = hub.reload(profile);
  layerc::test::expect(result.state == editor::ReloadState::failed &&
                           !result.error.empty() && starts == 0 && signals == 1,
                       "a refused peer fails, signals fallback and never launches");
}

void nullopt_reload_reports_signal_and_unavailable_results() {
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  {
    editor::DaemonHub hub(make_environment(private_pipe_name(), starts, signals));
    const auto result = hub.reload(std::nullopt);
    layerc::test::expect(result.state == editor::ReloadState::signalled && signals == 1,
                         "a delivered disk reload signal is reported as signalled");
  }
  std::atomic<unsigned> no_starts{};
  std::atomic<unsigned> failed_signals{};
  auto environment = make_environment(private_pipe_name(), no_starts, failed_signals);
  environment.signal_reload = [&failed_signals] {
    ++failed_signals;
    return false;
  };
  editor::DaemonHub unavailable(std::move(environment));
  const auto result = unavailable.reload(std::nullopt);
  layerc::test::expect(result.state == editor::ReloadState::daemon_unavailable &&
                           failed_signals == 1,
                       "a failed disk reload signal is reported unavailable");
}

void status_reply_is_parsed_and_windows_summary_is_neutral() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto status = hub.status();
  const auto summary = status.summary();
  layerc::test::expect(status.running && status.version == "test-build" &&
                           status.sessions == 2 && status.capabilities.size() == 1 &&
                           status.capabilities.front().name == "overlay" &&
                           summary.find("LayerD test-build — 2 sessions") != std::string::npos &&
                           summary.find("X11") == std::string::npos,
                       "status parses diagnostics and omits Linux X11 wording");
}

void held_connection_serves_later_requests_without_rehandshake() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto first = hub.status();
  const auto second = hub.status();
  layerc::test::expect(first.running && second.running && second.error.empty() &&
                           server.wait_for_diagnostics(2, 1s) &&
                           server.handshakes() == 1,
                       "the authenticated connection is kept for the next request");
}

void second_editor_reports_a_busy_pipe() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub holder(make_environment(server.pipe_name(), starts, signals));
  if (!layerc::test::expect(holder.status().running,
                            "the first editor holds LayerD's single live pipe"))
    return;
  editor::DaemonHub second(make_environment(server.pipe_name(), starts, signals, 700ms));
  const auto status = second.status();
  layerc::test::expect(!status.running && !status.socket_missing &&
                           status.error.find("busy") != std::string::npos,
                       "a second editor reports the held pipe instead of a missing daemon");
  layerc::test::TempDirectory temporary("layerc-p2b1-busy");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  const auto reload = second.reload(profile);
  layerc::test::expect(reload.state == editor::ReloadState::failed &&
                           reload.error.find("busy") != std::string::npos &&
                           starts == 0 && signals == 1,
                       "a busy pipe signals the disk reload but never claims it was applied");
}

void shutdown_joins_with_a_pending_status_demand() {
  FakeLayerD server(ReplyMode::silent);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals, 2s));
  std::promise<editor::DaemonStatus> result_promise;
  auto result = result_promise.get_future();
  std::thread caller([&] { result_promise.set_value(hub.status()); });
  const bool request_received = server.wait_for_diagnostics(1, 1s);
  layerc::test::expect(request_received,
                       "the fake daemon received the pending status demand");
  const auto began = std::chrono::steady_clock::now();
  hub.shutdown();
  const bool unblocked = result.wait_for(1s) == std::future_status::ready;
  layerc::test::expect(unblocked &&
                           std::chrono::steady_clock::now() - began < 1s,
                       "shutdown fails the pending caller and joins promptly");
  caller.join();
  if (unblocked) {
    const auto status = result.get();
    layerc::test::expect(!status.running && !status.error.empty(),
                         "shutdown returns a truthful unavailable status");
  }
}

void hub_trace_subscribe_acknowledges_with_reset() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  const bool requested = server.wait_for_trace_subscribes(1, 1s);
  layerc::test::expect(requested && server.trace_subscribe_count() == 1 &&
                           reset.has_value() && reset->reply.has_value() &&
                           reset->reply->reset && reset->reply->packet.empty() &&
                           reset->reply->generation == id,
                       "one acknowledged subscribe produces a reset reply");
  hub.unsubscribe_trace(id);
}

void hub_trace_receives_a_push_while_idle_connected() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                               reset->reply->reset,
                           "the initial trace reset is consumed before the push"))
    return;

  const std::vector<core::AutomationEvent> events{make_trace_event(1)};
  layerc::test::expect(server.push_trace(trace_packet(events)),
                       "the fake server queues a trace event");
  const auto update = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(update.has_value() && update->reply.has_value(),
                            "an idle-connected push reaches take_trace"))
    return;
  const auto parsed = core::parse_layerc_trace(update->reply->packet);
  const auto *delivered =
      std::get_if<std::vector<core::AutomationEvent>>(&parsed);
  layerc::test::expect(delivered != nullptr && delivered->size() == 1 &&
                           delivered->front().source_id == "hub-event-1" &&
                           update->reply->cursor == 1,
                       "the pushed event arrives without a pending request");
  hub.unsubscribe_trace(id);
}

void hub_trace_unsubscribe_stops_later_pushes() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                               reset->reply->reset,
                           "the trace subscription is established"))
    return;
  hub.unsubscribe_trace(id);
  const bool unsubscribed = server.wait_for_trace_unsubscribes(1, 1s);
  layerc::test::expect(unsubscribed && server.trace_unsubscribe_count() == 1,
                       "one unsubscribe is sent to the private server");
  const std::vector<core::AutomationEvent> events{make_trace_event(2)};
  layerc::test::expect(server.push_trace(trace_packet(events)),
                       "the fake server queues a post-unsubscribe trace");
  bool delivered{};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (hub.take_trace(id).has_value()) {
      delivered = true;
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  layerc::test::expect(!delivered,
                       "a post-unsubscribe push is not delivered to the old id");
}

void unconfirmed_unsubscribe_closes_the_trace_connection() {
  FakeLayerD server(ReplyMode::silent_trace_unsubscribe);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals, 700ms));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset && reset->reply && reset->reply->reset,
                           "the trace subscription is established"))
    return;

  hub.unsubscribe_trace(id);
  if (!layerc::test::expect(server.wait_for_trace_unsubscribes(1, 1s) &&
                               server.wait_for_disconnect(2s),
                           "an unconfirmed unsubscribe closes the pipe so recording cannot remain active"))
    return;
  const auto status = hub.status();
  layerc::test::expect(status.running && status.error.empty() &&
                           server.handshakes() == 2,
                       "the next status demand reconnects after trace cleanup");
}

void absent_trace_retries_three_times_without_starting_layerd() {
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(private_pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  std::optional<editor::AutomationTraceUpdate> failure;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto update = hub.take_trace(id)) {
      failure = std::move(update);
      break;
    }
    hub.poll_trace(id);
    std::this_thread::sleep_for(20ms);
  }
  if (!layerc::test::expect(failure.has_value(),
                            "three absent-pipe attempts produce an update"))
    return;
  layerc::test::expect(!failure->reply.has_value() &&
                           failure->error.find("LayerD is not running") !=
                               std::string::npos &&
                           starts == 0,
                       "an absent trace subscription reports LayerD without launching it");
}

void trace_subscribe_error_retries_with_layerd_text() {
  FakeLayerD server(ReplyMode::error);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  std::optional<editor::AutomationTraceUpdate> failure;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto update = hub.take_trace(id)) {
      failure = std::move(update);
      break;
    }
    hub.poll_trace(id);
    std::this_thread::sleep_for(20ms);
  }
  layerc::test::expect(failure.has_value() && !failure->reply.has_value() &&
                           failure->error == "test trace subscribe rejected" &&
                           server.trace_subscribe_count() == 3 && starts == 0,
                       "three private error replies end the subscription without launching");
}

void disconnected_trace_resubscribes_with_a_gap() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                               reset->reply->reset,
                           "the first trace subscription is established"))
    return;
  if (!layerc::test::expect(server.disconnect_client(),
                            "the fake server requests a client disconnect"))
    return;

  std::optional<editor::AutomationTraceUpdate> retried;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    hub.poll_trace(id);
    if (auto update = hub.take_trace(id)) {
      retried = std::move(update);
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  const bool resubscribed = server.wait_for_trace_subscribes(2, 1s);
  layerc::test::expect(resubscribed && server.trace_subscribe_count() == 2 &&
                           retried.has_value() && retried->reply.has_value() &&
                           retried->reply->gap && !retried->reply->reset,
                       "the next poll resubscribes and reports a trace gap");
  hub.unsubscribe_trace(id);
}

void shutdown_while_trace_is_held_reports_an_error() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                               reset->reply->reset,
                           "the trace subscription is established before shutdown"))
    return;
  hub.shutdown();
  const auto stopped = hub.take_trace(id);
  layerc::test::expect(stopped.has_value() && !stopped->reply.has_value() &&
                           stopped->error == "The LayerD link was shut down.",
                       "shutdown ends the held trace with the documented error");
}

void status_reload_and_trace_continue_on_one_held_connection() {
  FakeLayerD server(ReplyMode::acknowledge);
  if (!layerc::test::expect(server.start(), "the private fake LayerD pipe is ready"))
    return;
  std::atomic<unsigned> starts{};
  std::atomic<unsigned> signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(reset.has_value() && reset->reply.has_value() &&
                               reset->reply->reset,
                           "the trace subscription is held before status and reload"))
    return;
  const auto status = hub.status();
  layerc::test::TempDirectory temporary("layerc-p2b2-trace-reload");
  const auto profile = make_committed_profile(temporary);
  if (!layerc::test::expect(profile.has_value(), "a committed profile fixture is written"))
    return;
  const auto reload = hub.reload(profile);
  const std::vector<core::AutomationEvent> events{make_trace_event(3)};
  layerc::test::expect(server.push_trace(trace_packet(events)),
                       "the fake server queues a trace after status and reload");
  const auto update = wait_for_trace_update(hub, id);
  if (!layerc::test::expect(update.has_value() && update->reply.has_value(),
                            "trace events continue flowing after status and reload"))
    return;
  const auto parsed = core::parse_layerc_trace(update->reply->packet);
  const auto *delivered =
      std::get_if<std::vector<core::AutomationEvent>>(&parsed);
  layerc::test::expect(status.running && status.error.empty() &&
                           reload.state == editor::ReloadState::acknowledged &&
                           delivered != nullptr && delivered->size() == 1 &&
                           delivered->front().source_id == "hub-event-3" &&
                           starts == 0 && signals == 0,
                       "status, committed reload, and trace share the held hub");
  hub.unsubscribe_trace(id);
}

std::vector<editor::OnWindowEditEvent> wait_for_edit_event(
    editor::DaemonHub &hub, const std::uint64_t id, const editor::OnWindowEditEvent::Kind kind,
    const std::chrono::milliseconds timeout = 2s) {
  std::vector<editor::OnWindowEditEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    bool found{};
    for (auto &event : hub.take_edit_events(id)) {
      found = found || event.kind == kind;
      events.push_back(std::move(event));
    }
    if (found) break;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return events;
}

std::uint64_t begin_on_desktop(editor::DaemonHub &hub) {
  return hub.begin_edit(reinterpret_cast<std::uintptr_t>(GetDesktopWindow()),
                        {"position-profile", "bar", "{}"});
}

void hub_edit_heartbeats_and_multiplexes() {
  using Type = core::LiveMessageType;
  using Kind = editor::OnWindowEditEvent::Kind;
  using layerc::test::expect;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!expect(server.start(), "private fake LayerD listens")) return;
  std::atomic<unsigned> starts{}, signals{}, notifications{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  hub.set_edit_listener([&] { ++notifications; });
  const auto id = begin_on_desktop(hub);
  const auto events = wait_for_edit_event(hub, id, Kind::anchor);
  if (!expect(events.size() == 4 && events[0].kind == Kind::awaiting &&
                  events[1].kind == Kind::started && events[2].kind == Kind::acknowledged &&
                  events[3].kind == Kind::anchor && events[3].final && events[3].x == .2,
              "begin, acknowledgement and pushed anchor arrive in order")) return;
  const auto received = server.edit_packets();
  expect(!received.empty() && received.front().message.type == Type::begin_edit &&
             received.front().message.target_window == reinterpret_cast<std::uintptr_t>(GetDesktopWindow()) &&
             received.front().message.profile_key == "position-profile" &&
             received.front().message.panel_id == "bar" && received.front().message.profile_json == "{}",
         "private LayerD receives the begin payload");
  expect(hub.status().running, "status answers while editing");
  layerc::test::TempDirectory temporary("layerc-p2h-edit-reload");
  const auto profile = make_committed_profile(temporary);
  expect(profile && hub.reload(profile).state == editor::ReloadState::acknowledged,
         "committed reload answers while editing");
  const auto trace = hub.subscribe_trace();
  const auto reset = wait_for_trace_update(hub, trace);
  expect(reset && reset->reply && reset->reply->reset, "trace subscribes while editing");
  expect(server.handshakes() == 1 && starts == 0 && signals == 0,
         "the active edit and requests share one authenticated pipe");
  expect(server.wait_for_edit_packets(Type::heartbeat, 3, 1500ms), "three edit heartbeats arrive");
  std::optional<std::chrono::steady_clock::time_point> previous;
  for (const auto &packet : server.edit_packets()) {
    if (packet.message.type != Type::heartbeat) continue;
    if (previous) expect(packet.received - *previous <= 300ms, "heartbeat intervals are at most 300 ms");
    previous = packet.received;
  }
  hub.end_edit(id, true);
  if (!expect(server.wait_for_edit_packets(Type::end_edit, 1, 1s), "manual end reaches LayerD")) return;
  const auto idle_packets = server.edit_packets().size();
  const auto idle_notifications = notifications.load();
  std::this_thread::sleep_for(350ms);
  expect(server.edit_packets().size() == idle_packets && notifications == idle_notifications,
         "idle link sends no heartbeats or edit notifications after end");
}

// layer57 L57-01: the hub reports LayerD's answer to a Direct drag off, and
// ends the edit when LayerD never answers so no heartbeat keeps it alive.
void hub_edit_direct_drag_off_needs_an_answer() {
  using Type = core::LiveMessageType;
  using Event = editor::OnWindowEditEvent;
  using layerc::test::expect;
  for (const auto mode : {ReplyMode::acknowledge, ReplyMode::silent}) {
    const bool answers = mode == ReplyMode::acknowledge;
    FakeLayerD server(mode);
    server.hold_off_ack(true);
    if (!expect(server.start(), "private fake LayerD listens")) return;
    std::atomic<unsigned> starts{}, signals{};
    editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
    const auto id = begin_on_desktop(hub);
    if (!expect(!wait_for_edit_event(hub, id, Event::Kind::anchor).empty(),
                "the edit starts")) return;
    expect(hub.direct_drag_released(), "a session that has not enabled Direct drag is released");
    hub.edit_direct_drag(id, true);
    expect(!hub.direct_drag_released(), "an enqueued enable makes Direct drag unreleased");
    hub.edit_direct_drag(id, false);
    if (!expect(server.wait_for_edit_packets(Type::set_direct_drag, 2, 1s),
                "both Direct drag requests reach LayerD")) return;
    expect(!hub.direct_drag_released(), "sending off does not release before its answer");
    if (answers) {
      server.hold_off_ack(false);
      const auto events = wait_for_edit_event(hub, id, Event::Kind::direct_drag_off);
      expect(hub.direct_drag_released() && !events.empty() && events.back().kind == Event::Kind::direct_drag_off &&
                 std::ranges::none_of(events, [](const Event &event) {
                   return event.kind == Event::Kind::ended;
                 }),
             "LayerD's answer confirms the off and keeps the edit");
      hub.end_edit(id, true);
      expect(server.wait_for_edit_packets(Type::end_edit, 1, 1s), "the edit then ends normally");
      continue;
    }
    const auto events = wait_for_edit_event(hub, id, Event::Kind::ended, 3s);
    if (!expect(!events.empty() && events.back().kind == Event::Kind::ended &&
                    events.back().reason == Event::EndReason::direct_drag_unconfirmed,
                "an unanswered off ends the edit as unconfirmed") ||
        !expect(server.wait_for_edit_packets(Type::end_edit, 1, 1s),
                "the hub tells LayerD the edit ended")) return;
    const auto packets = server.edit_packets().size();
    std::this_thread::sleep_for(350ms);
    expect(server.edit_packets().size() == packets && !hub.direct_drag_released(),
           "no heartbeat or release proof follows an unanswered off");
  }
}

bool until(const auto &condition, const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

void hub_enable_enqueued_during_release_publication() {
  using Kind = editor::OnWindowEditEvent::Kind;
  using layerc::test::expect;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!expect(server.start(), "private fake LayerD listens")) return;
  std::atomic<unsigned> starts{}, signals{};
  std::atomic<bool> queued{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = begin_on_desktop(hub);
  if (!expect(!wait_for_edit_event(hub, id, Kind::anchor).empty(), "the edit starts")) return;
  editor::DaemonHubTestAccess::before_release_publish(hub, [&] {
    hub.edit_direct_drag(id, true);
    queued = true;
  });
  expect(hub.status().running && queued &&
             server.wait_for_edit_packets(core::LiveMessageType::set_direct_drag, 1, 1s) &&
             !hub.direct_drag_released(),
         "publication cannot release an enable queued after the worker swapped commands");
}

void hub_unsent_off_keeps_idle_session_until_capable_reconnect() {
  using Type = core::LiveMessageType;
  using Event = editor::OnWindowEditEvent;
  using layerc::test::expect;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!expect(server.start(), "private fake LayerD listens")) return;
  std::atomic<unsigned> starts{}, signals{}, carries{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  editor::DaemonHubTestAccess::on_release_carried(hub, [&](editor::DirectDragRelease &) { ++carries; });
  const auto id = begin_on_desktop(hub);
  if (!expect(!wait_for_edit_event(hub, id, Event::Kind::anchor).empty(), "the edit starts")) return;
  hub.edit_direct_drag(id, true);
  if (!expect(server.wait_for_edit_packets(Type::set_direct_drag, 1, 1s) && server.pause_reads(true),
              "LayerD receives the enable, then stops draining the pipe")) return;
  server.release_proof_capability(false);
  // Final anchors neither coalesce nor throttle. Fill the pipe and queue,
  // then use smaller frames to fill the space that would fit the short off.
  const std::string panel(core::max_live_message_bytes * 3U / 4U, 'p');
  for (unsigned index = 0; index < 8; ++index) hub.edit_anchor(id, panel, .5, .5, true);
  for (auto size = panel.size() / 2U; size != 0; size /= 2U)
    hub.edit_anchor(id, panel.substr(0, size), .5, .5, true);
  // A heartbeat is even shorter than an off; give the worker several
  // intervals to fill any space left below the smallest anchor frame.
  std::this_thread::sleep_for(750ms);
  hub.edit_direct_drag(id, false);
  const auto events = wait_for_edit_event(hub, id, Event::Kind::ended);
  if (!expect(!events.empty() && events.back().kind == Event::Kind::ended &&
                  events.back().reason == Event::EndReason::direct_drag_unsent &&
                  !hub.direct_drag_released(),
              "a full channel makes off unsent without treating the drag as released")) return;
  const bool reconnected = server.pause_reads(false) && server.wait_for_disconnect(2s) &&
                          until([&] { return server.handshakes() >= 2; }) && hub.status().running;
  expect(carries == 0, "the idle worker polls a pending release without discarding its session");
  if (!expect(reconnected, "the reset closes the pipe and the idle hub authenticates again")) return;
  expect(!hub.direct_drag_released(), "an ack without release-proof-v1 does not release");
  server.release_proof_capability(true);
  expect(server.disconnect_client() && until([&] { return hub.direct_drag_released(); }, 3s) &&
             server.handshakes() >= 3 && starts == 0,
         "only the fresh capable handshake releases the drag after an unsent off");
}

void hub_discarded_drag_release_uses_reconnect_or_peer_exit() {
  using Kind = editor::OnWindowEditEvent::Kind;
  using layerc::test::expect;
  for (const bool peer_exits : {false, true}) {
    FakeLayerD server(ReplyMode::silent_trace_unsubscribe);
    if (!expect(server.start(), "private fake LayerD listens")) return;
    std::atomic<unsigned> starts{}, signals{}, carries{};
    std::atomic<bool> exited{};
    editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals, 800ms));
    editor::DaemonHubTestAccess::on_release_carried(hub, [&](editor::DirectDragRelease &release) {
      if (peer_exits) release.probe = [&] { return exited.load(); };
      ++carries;
    });
    const auto id = begin_on_desktop(hub);
    if (!expect(!wait_for_edit_event(hub, id, Kind::anchor).empty(), "the edit starts")) return;
    hub.edit_direct_drag(id, true);
    if (!expect(server.wait_for_edit_packets(core::LiveMessageType::set_direct_drag, 1, 1s),
                "the first connection carried an enable")) return;
    const auto trace = hub.subscribe_trace();
    if (!expect(wait_for_trace_update(hub, trace).has_value(), "trace subscribes while editing")) return;
    hub.unsubscribe_trace(trace);
    if (!expect(server.wait_for_trace_unsubscribes(1, 1s), "trace unsubscribe awaits an answer")) return;
    server.stop();
    if (!expect(until([&] { return carries.load() != 0; }), "discarding the connection carries its armed release")) return;
    expect(!until([&] { return hub.direct_drag_released(); }, 350ms),
           "a carried release keeps Pick unavailable with no session or live pipe");
    if (peer_exits) {
      exited = true;
      expect(until([&] { return hub.direct_drag_released(); }) && server.handshakes() == 1 && starts == 0,
             "the carried peer-exit probe releases without a handshake or launch");
    } else {
      expect(server.start() && until([&] { return hub.direct_drag_released(); }) &&
                 server.handshakes() >= 2 && starts == 0,
             "a discarded armed release reconnects to the existing pipe without launching LayerD");
    }
  }
}

void hub_end_survives_wake_failure() {
  using Type = core::LiveMessageType;
  using Kind = editor::OnWindowEditEvent::Kind;
  using layerc::test::expect;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!expect(server.start(), "private fake LayerD listens")) return;
  std::atomic<unsigned> starts{}, signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto id = begin_on_desktop(hub);
  if (!expect(!wait_for_edit_event(hub, id, Kind::anchor).empty() &&
                  server.wait_for_edit_packets(Type::heartbeat, 1, 1s),
              "the edit is sending heartbeats before the wake failure")) return;
  editor::DaemonHubTestAccess::fail_edit_wake(hub);
  hub.end_edit(id, true);
  if (!expect(server.wait_for_edit_packets(Type::end_edit, 1, 1s),
              "an end whose wake failed still ends the driver session")) return;
  const auto count = server.edit_packets().size();
  std::this_thread::sleep_for(350ms);
  expect(server.edit_packets().size() == count, "a failed end wake leaves no more heartbeats");
}

void hub_edit_expires_without_server() {
  using Kind = editor::OnWindowEditEvent::Kind;
  using layerc::test::expect;
  std::atomic<unsigned> starts{}, signals{};
  editor::DaemonHub hub(make_environment(private_pipe_name(), starts, signals));
  const auto id = begin_on_desktop(hub);
  const auto events = wait_for_edit_event(hub, id, Kind::expired, 7s);
  expect(events.size() == 2 && events.front().kind == Kind::awaiting &&
             events.back().kind == Kind::expired && starts == 1 && signals == 0,
         "an absent private pipe requests one launch and expires without authenticating");
}

void hub_edit_disconnect_and_shutdown() {
  using Event = editor::OnWindowEditEvent;
  using layerc::test::expect;
  FakeLayerD server(ReplyMode::acknowledge);
  if (!expect(server.start(), "private fake LayerD listens")) return;
  const auto threads = process_thread_count();
  std::atomic<unsigned> starts{}, signals{};
  editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
  const auto first = begin_on_desktop(hub);
  auto events = wait_for_edit_event(hub, first, Event::Kind::anchor);
  if (!expect(!events.empty() && events.back().kind == Event::Kind::anchor,
              "the first connection acknowledges and pushes an anchor") ||
      !expect(server.disconnect_client(), "force disconnect only on the private pipe")) return;
  events = wait_for_edit_event(hub, first, Event::Kind::ended);
  expect(!events.empty() && events.back().kind == Event::Kind::ended &&
             events.back().reason == Event::EndReason::connection_lost,
         "server disconnect produces connection_lost");
  const auto next = begin_on_desktop(hub);
  events = wait_for_edit_event(hub, next, Event::Kind::anchor);
  if (!expect(!events.empty() && events.back().kind == Event::Kind::anchor,
              "a new edit begins on a fresh connection")) return;
  const auto before = server.disconnects();
  hub.shutdown();
  // The end_edit is best-effort: a frame read in the poll that sees the close
  // is discarded with its connection, and LayerD ends the session on the close.
  expect(server.wait_for_disconnect(1s, before + 1) && process_thread_count() == threads,
         "shutdown closes the private daemon's connection and joins the worker");
}

void editor_image_allowlist_is_exact() {
  const auto &names = platform::layerc_editor_image_names;
  const bool accepts_windows = names.size() == 2 &&
      std::wstring_view(names[0]) == L"layere.exe" &&
      std::wstring_view(names[1]) == L"layere_fltk.exe";
  const auto contains = [](const std::wstring_view image) {
    for (const auto *name : platform::layerc_editor_image_names)
      if (std::wstring_view(name) == image) return true;
    return false;
  };
  layerc::test::expect(accepts_windows && contains(L"layere.exe") &&
                           contains(L"layere_fltk.exe") &&
                           !contains(L"layerd.exe") && !contains(L"other.exe"),
                       "the editor trust allowlist accepts only both editor images");
}

void hub_retry_visual_and_automation_replies() {
  using enum editor::DaemonCommandState;
  using layerc::test::expect;
  layerc::test::TempDirectory temporary("layerc-retry");
  const auto profile = make_committed_profile(temporary);
  if (!expect(profile.has_value(), "create a private retry profile")) return;
  std::atomic<unsigned> starts{}, signals{};
  for (const auto mode : {ReplyMode::acknowledge, ReplyMode::error}) {
    FakeLayerD server(mode);
    if (!expect(server.start(), "start the private retry fixture")) return;
    editor::DaemonHub hub(make_environment(server.pipe_name(), starts, signals));
    const bool accepts = mode == ReplyMode::acknowledge;
    const auto visual = hub.retry_visual();
    const auto automation = hub.retry_automation(profile->path, "test-automation");
    expect(visual.state == (accepts ? acknowledged : refused) &&
               visual.text == (accepts ? "visual runtime retry requested" : "test retry refused"),
           "visual retry preserves the correlated reply");
    expect(automation.state == (accepts ? acknowledged : refused) &&
               automation.text == (accepts ? "automation retry requested" : "test retry refused"),
           "automation retry preserves the correlated reply");
    const auto packets = server.retry_packets();
    expect(packets.size() == 2 &&
               packets[0].type == core::LiveMessageType::retry_visual &&
               packets[1].type == core::LiveMessageType::retry_automation &&
               packets[1].profile_key == platform::canonical_profile_key(profile->path) &&
               packets[1].text == "test-automation" && server.handshakes() == 1,
           "retries share the authenticated pipe and send the canonical automation identity");
  }
  editor::DaemonHub absent(make_environment(private_pipe_name(), starts, signals));
  expect(absent.retry_visual().state == daemon_unavailable &&
             absent.retry_automation(profile->path, "test-automation").state == daemon_unavailable &&
             starts == 0 && signals == 0,
         "retry never launches or signals LayerD when its private pipe is absent");
}

} // namespace

LAYERC_TEST_MAIN(
     LAYERC_TEST_CASE(hub_retry_visual_and_automation_replies),
    LAYERC_TEST_CASE(lazy_worker_and_absent_status),
    LAYERC_TEST_CASE(absent_reload_starts_once_without_claiming_success),
    LAYERC_TEST_CASE(acknowledgement_confirms_committed_reload),
    LAYERC_TEST_CASE(error_reply_fails_with_layerd_text),
    LAYERC_TEST_CASE(absent_reply_times_out_within_deadline),
    LAYERC_TEST_CASE(rejected_client_fails_and_signals_without_launching),
    LAYERC_TEST_CASE(nullopt_reload_reports_signal_and_unavailable_results),
    LAYERC_TEST_CASE(status_reply_is_parsed_and_windows_summary_is_neutral),
    LAYERC_TEST_CASE(held_connection_serves_later_requests_without_rehandshake),
    LAYERC_TEST_CASE(second_editor_reports_a_busy_pipe),
    LAYERC_TEST_CASE(shutdown_joins_with_a_pending_status_demand),
    LAYERC_TEST_CASE(hub_trace_subscribe_acknowledges_with_reset),
    LAYERC_TEST_CASE(hub_trace_receives_a_push_while_idle_connected),
    LAYERC_TEST_CASE(hub_trace_unsubscribe_stops_later_pushes),
    LAYERC_TEST_CASE(unconfirmed_unsubscribe_closes_the_trace_connection),
    LAYERC_TEST_CASE(absent_trace_retries_three_times_without_starting_layerd),
    LAYERC_TEST_CASE(trace_subscribe_error_retries_with_layerd_text),
    LAYERC_TEST_CASE(disconnected_trace_resubscribes_with_a_gap),
    LAYERC_TEST_CASE(shutdown_while_trace_is_held_reports_an_error),
    LAYERC_TEST_CASE(status_reload_and_trace_continue_on_one_held_connection),
    LAYERC_TEST_CASE(hub_edit_heartbeats_and_multiplexes),
    LAYERC_TEST_CASE(hub_edit_direct_drag_off_needs_an_answer),
    LAYERC_TEST_CASE(hub_enable_enqueued_during_release_publication),
    LAYERC_TEST_CASE(hub_unsent_off_keeps_idle_session_until_capable_reconnect),
    LAYERC_TEST_CASE(hub_discarded_drag_release_uses_reconnect_or_peer_exit),
    LAYERC_TEST_CASE(hub_end_survives_wake_failure),
    LAYERC_TEST_CASE(hub_edit_expires_without_server),
    LAYERC_TEST_CASE(hub_edit_disconnect_and_shutdown),
    LAYERC_TEST_CASE(editor_image_allowlist_is_exact))
