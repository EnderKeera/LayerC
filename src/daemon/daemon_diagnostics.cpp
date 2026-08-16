#include "daemon_app.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/text.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include <psapi.h>
#include <tlhelp32.h>

namespace layerc::daemon {
namespace {

const char *state_name(const layerc::runtime::ServiceState state) noexcept {
  switch (state) {
  case layerc::runtime::ServiceState::absent:
    return "absent";
  case layerc::runtime::ServiceState::active:
    return "active";
  case layerc::runtime::ServiceState::faulted:
    return "faulted";
  }
  return "unknown";
}

std::uint64_t file_time_value(const FILETIME value) noexcept {
  return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
         value.dwLowDateTime;
}

std::uint32_t process_thread_count() noexcept {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;
  THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
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

template <typename Integer>
void append_number_field(std::string &output, const std::string_view name,
                         const Integer value, bool &first) {
  if (!first)
    output.push_back(',');
  first = false;
  layerc::core::json::append_quoted(output, name);
  output.push_back(':');
  layerc::core::json::append_unsigned(output,
                                      static_cast<std::uint64_t>(value));
}

void append_time(std::string &output, const std::string_view name,
                 const std::optional<layerc::core::TimePoint> value,
                 bool &first) {
  if (!first)
    output.push_back(',');
  first = false;
  layerc::core::json::append_quoted(output, name);
  output.push_back(':');
  if (!value) {
    output += "null";
    return;
  }
  const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         value->time_since_epoch())
                         .count();
  layerc::core::json::append_unsigned(
      output, ticks < 0 ? 0U : static_cast<std::uint64_t>(ticks));
}

void append_resources(std::string &output,
                      const layerc::runtime::RuntimeResourceStats &resources) {
  output.push_back('{');
  bool first = true;
  append_number_field(output, "threads", resources.threads, first);
  append_number_field(output, "timers", resources.timers, first);
  append_number_field(output, "handles", resources.handles, first);
  append_number_field(output, "windows", resources.windows, first);
  append_number_field(output, "objects", resources.objects, first);
  append_number_field(output, "captureSessions", resources.capture_sessions,
                      first);
  append_number_field(output, "queues", resources.queues, first);
  append_number_field(output, "decodedAssets", resources.decoded_assets, first);
  append_number_field(output, "cacheBytes", resources.cache_bytes, first);
  output.push_back('}');
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

  std::string output;
  output.reserve(8U * 1024U);
  output +=
      "{\"schemaVersion\":1,\"packageVersion\":\"2.3.0\",\"protocolVersion\":";
  layerc::core::json::append_unsigned(output,
                                      layerc::core::live_protocol_version);
  output += ",\"process\":{";
  bool first = true;
  append_number_field(output, "pid", GetCurrentProcessId(), first);
  append_number_field(output, "privateBytes",
                      have_memory ? memory.PrivateUsage : 0, first);
  append_number_field(output, "workingSetBytes",
                      have_memory ? memory.WorkingSetSize : 0, first);
  append_number_field(output, "handles", handles, first);
  append_number_field(output, "threads", process_thread_count(), first);
  append_number_field(output, "kernelTime100ns",
                      have_times ? file_time_value(kernel) : 0, first);
  append_number_field(output, "userTime100ns",
                      have_times ? file_time_value(user) : 0, first);
  output += ",\"gpuAllocation\":{\"available\":false,\"reason\":\"adapter "
            "budget telemetry unavailable\"}}";

  output += ",\"services\":[";
  const auto services = runtime_kernel_.services().all_stats();
  for (std::size_t index = 0; index < services.size(); ++index) {
    const auto &service = services[index];
    if (index != 0)
      output.push_back(',');
    output += "{\"capability\":";
    layerc::core::json::append_quoted(
        output, layerc::runtime::capability_name(service.capability));
    output += ",\"state\":";
    layerc::core::json::append_quoted(output, state_name(service.state));
    output += ",\"consumers\":";
    layerc::core::json::append_unsigned(output, service.consumers);
    output += ",\"resources\":";
    append_resources(output, service.resources);
    output += ",\"activationCount\":";
    layerc::core::json::append_unsigned(output, service.activation_count);
    output += ",\"faultCount\":";
    layerc::core::json::append_unsigned(output, service.fault_count);
    bool time_first = false;
    append_time(output, "lastActivationNs", service.last_activation,
                time_first);
    append_time(output, "lastQuiescenceNs", service.last_quiescence,
                time_first);
    output += ",\"lastError\":";
    layerc::core::json::append_quoted(output, service.last_error);
    output.push_back('}');
  }
  output.push_back(']');
  output += ",\"aggregateOwnedResources\":";
  append_resources(output, runtime_kernel_.services().aggregate_resources());

  output += ",\"sessions\":[";
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  for (std::size_t index = 0; index < sessions_.size(); ++index) {
    const auto &session = *sessions_[index];
    if (index != 0)
      output.push_back(',');
    output += "{\"profileId\":";
    layerc::core::json::append_quoted(output, session.profile.id);
    output += ",\"sourceKey\":";
    layerc::core::json::append_quoted(
        output, layerc::platform::wide_to_utf8(session.source_key));
    output += ",\"window\":\"";
    layerc::core::json::append_unsigned(
        output, reinterpret_cast<std::uintptr_t>(session.game_window));
    output += "\",\"foreground\":";
    output +=
        session.game_window != nullptr && session.game_window == foreground
            ? "true"
            : "false";
    output += ",\"overlayWindows\":";
    layerc::core::json::append_unsigned(output,
                                        session.overlays.object_count());
    output += ",\"lastInputMatch\":";
    layerc::core::json::append_quoted(output, session.last_input_match);
    output.push_back('}');
  }
  output += "]}";
  output.pop_back();
  output += ",\"isolatedHelpers\":[";
  bool first_helper = true;
  for (const auto &[window, worker] : isolated_visual_captures_) {
    if (!first_helper)
      output.push_back(',');
    first_helper = false;
    const auto status = worker->status();
    output += "{\"window\":\"";
    layerc::core::json::append_unsigned(
        output, reinterpret_cast<std::uintptr_t>(window));
    output += "\",\"pid\":";
    layerc::core::json::append_unsigned(output, status.helper_process_id);
    output += status.running ? ",\"running\":true" : ",\"running\":false";
    output +=
        status.quarantined ? ",\"quarantined\":true" : ",\"quarantined\":false";
    output += ",\"faultCount\":";
    layerc::core::json::append_unsigned(output, status.fault_count);
    output += ",\"lastError\":";
    layerc::core::json::append_quoted(output, status.last_error);
    output.push_back('}');
  }
  output += "]}";
  return output;
}

} // namespace layerc::daemon
