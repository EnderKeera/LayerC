#include "platform/win32/daemon_hub.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/runtime/diagnostics.hpp"
#include "layerc/runtime/runtime_services.hpp"
#include "test_support.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {
using namespace std::chrono_literals;
namespace core = layerc::core;
namespace editor = layerc::editor;
namespace platform = layerc::platform;
namespace runtime = layerc::runtime;

std::wstring private_pipe_name() {
  static std::atomic<std::uint64_t> sequence{};
  std::wstring name = L"\\\\.\\pipe\\LayerC.P2B1.Tests.";
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

enum class ReplyMode { acknowledge, error, silent };

class FakeLayerD final {
public:
  explicit FakeLayerD(const ReplyMode mode,
                      const DWORD expected_client_pid = GetCurrentProcessId())
      : pipe_name_(private_pipe_name()), mode_(mode),
        expected_client_pid_(expected_client_pid),
        stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        diagnostics_json_(test_diagnostics_json()) {}

  ~FakeLayerD() {
    if (stop_event_ != nullptr) (void)SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    if (stop_event_ != nullptr) CloseHandle(stop_event_);
  }

  FakeLayerD(const FakeLayerD &) = delete;
  FakeLayerD &operator=(const FakeLayerD &) = delete;

  [[nodiscard]] bool start() {
    if (stop_event_ == nullptr) return false;
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

private:
  void reply(platform::NamedPipeEditorDaemonChannel &channel,
             const core::LiveMessage &request, const core::LiveMessageType type,
             std::string text = {}) {
    core::LiveMessage message;
    message.type = type;
    message.revision = request.revision;
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
            core::package_identity("LayerD") + ";test");
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
    platform::NamedPipeEditorDaemonChannel channel(
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
      for (const auto &result : channel.poll()) {
        if (const auto *message = std::get_if<core::LiveMessage>(&result))
          handle(channel, *message);
      }
      if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
      std::array<HANDLE, 4> handles{};
      handles[0] = stop_event_;
      const auto pipe_count = channel.wait_handles(
          std::span<HANDLE>(handles).subspan(1));
      const DWORD count = static_cast<DWORD>(pipe_count + 1U);
      const DWORD waited =
          WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);
      if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) break;
    }
  }

  std::wstring pipe_name_;
  ReplyMode mode_;
  DWORD expected_client_pid_{};
  HANDLE stop_event_{};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::condition_variable request_cv_;
  bool ready_{};
  bool pipe_ready_{};
  std::size_t diagnostics_count_{};
  std::size_t handshake_count_{};
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

void editor_image_allowlist_is_exact() {
  const auto &names = platform::layerc_editor_image_names;
  const bool accepts_windows = names.size() == 2 &&
      std::wstring_view(names[0]) == L"layere.exe" &&
      std::wstring_view(names[1]) == L"layere_fltk.exe";
  const auto contains = [&names](const std::wstring_view image) {
    for (const auto *name : names)
      if (std::wstring_view(name) == image) return true;
    return false;
  };
  layerc::test::expect(accepts_windows && contains(L"layere.exe") &&
                           contains(L"layere_fltk.exe") &&
                           !contains(L"layerd.exe") && !contains(L"other.exe"),
                       "the editor trust allowlist accepts only both editor images");
}

} // namespace

LAYERC_TEST_MAIN(
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
    LAYERC_TEST_CASE(editor_image_allowlist_is_exact))
