#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/runtime/diagnostics.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include <psapi.h>
#include <tlhelp32.h>

namespace layerc::daemon {
namespace {

std::uint64_t file_time_value(const FILETIME value) noexcept {
  return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
         value.dwLowDateTime;
}

std::uint32_t process_thread_count() noexcept {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::uint32_t count{};
  if (Thread32First(snapshot, &entry) != FALSE) {
    do {
      if (entry.th32OwnerProcessID == GetCurrentProcessId())
        ++count;
      entry.dwSize = sizeof(THREADENTRY32);
    } while (Thread32Next(snapshot, &entry) != FALSE);
  }
  CloseHandle(snapshot);
  return count;
}

} // namespace

std::string DaemonApp::runtime_diagnostics_json() const {
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  const bool have_memory =
      GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                           sizeof(memory)) != FALSE;
  DWORD handles{};
  (void)GetProcessHandleCount(GetCurrentProcess(), &handles);
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool have_times = GetProcessTimes(GetCurrentProcess(), &created,
                                          &exited, &kernel, &user) != FALSE;

  layerc::runtime::RuntimeDiagnostics diagnostics;
  diagnostics.process = {
      .pid = GetCurrentProcessId(),
      .private_bytes = have_memory ? memory.PrivateUsage : 0,
      .working_set_bytes = have_memory ? memory.WorkingSetSize : 0,
      .handles = handles,
      .threads = process_thread_count(),
      .kernel_time_100ns = have_times ? file_time_value(kernel) : 0,
      .user_time_100ns = have_times ? file_time_value(user) : 0,
  };
  const auto services = runtime_kernel_.services().all_stats();
  diagnostics.services.assign(services.begin(), services.end());
  diagnostics.gamebar = {
      .connected = companion_host_.transport().connected(),
      .last_error =
          layerc::platform::wide_to_utf8(companion_host_.transport().last_error()),
  };
  diagnostics.settings = {
      .capture_exclusion = capture_exclusion_,
      .visual_recognition = visual_recognition_enabled_,
      .last_error = settings_error_,
  };
  diagnostics.aggregate_resources =
      runtime_kernel_.services().aggregate_resources();

  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  diagnostics.sessions.reserve(session_registry_.sessions().size());
  for (const auto &entry : session_registry_.sessions()) {
    const auto &session = *entry;
    diagnostics.sessions.push_back({
        .profile_id = session.profile.id,
        .source_key = session.source_key,
        .window = reinterpret_cast<std::uintptr_t>(session.game_window),
        .foreground = session.game_window != nullptr &&
                      session.game_window == foreground,
        .overlay_windows =
            static_cast<std::uint32_t>(session.overlays.window_count()),
        .last_input_match = session.last_input_match,
    });
  }
  diagnostics.helpers.reserve(visual_capture_runtime_.captures().size());
  for (const auto &[window, worker] : visual_capture_runtime_.captures()) {
    if (worker == nullptr ||
        worker->runtime() != layerc::core::VisualRuntime::isolated)
      continue;
    const auto status = worker->status();
    diagnostics.helpers.push_back({
        .window = reinterpret_cast<std::uintptr_t>(window),
        .pid = status.helper_process_id,
        .running = status.running,
        .quarantined = status.quarantined,
        .fault_count = status.fault_count,
        .last_error = status.last_error,
    });
  }
  return layerc::runtime::serialize_runtime_diagnostics_json(diagnostics);
}

} // namespace layerc::daemon
