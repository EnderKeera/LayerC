#include "runtime_diagnostics_view.hpp"

#include "layerc/runtime/diagnostics.hpp"

#include <algorithm>
#include <cstdint>
#include <variant>

namespace layerc::editor {
namespace {
namespace runtime = layerc::runtime;

// First non-empty last_error among items, or empty when none reported one.
template <typename Items>
std::string first_last_error(const Items &items) {
  const auto found = std::ranges::find_if(
      items, [](const auto &item) { return !item.last_error.empty(); });
  return found == std::ranges::end(items) ? std::string{} : found->last_error;
}

std::string mebibytes(const std::uint64_t bytes) {
  // Quotient and remainder separately: no intermediate can overflow.
  constexpr std::uint64_t mib = 1024U * 1024U;
  auto whole = bytes / mib;
  auto tenths = ((bytes % mib) * 10U + mib / 2U) / mib;
  if (tenths == 10U) { ++whole; tenths = 0U; }
  return std::to_string(whole) + "." + std::to_string(tenths) + " MiB";
}

std::string hex(std::uint64_t value) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string text;
  do {
    text.insert(text.begin(), digits[value & 0xFU]);
    value >>= 4U;
  } while (value != 0U);
  return "0x" + text;
}

std::string extension(const std::string_view name, const runtime::X11ExtensionStatus &status) {
  std::string text(name);
  if (!status.available)
    return text + " unavailable" + (status.reason ? " (" + *status.reason + ")" : "");
  return text + " " + std::to_string(status.major) + "." + std::to_string(status.minor);
}

void append_details(std::vector<std::string> &lines, const runtime::RuntimeDiagnostics &diagnostics) {
  const auto &process = diagnostics.process;
  lines.push_back("");
  lines.push_back("LayerD " + diagnostics.package_version + ", diagnostics schema " +
                  std::to_string(diagnostics.schema_version) + ", process " +
                  std::to_string(process.pid));
  lines.push_back("Memory: private " + mebibytes(process.private_bytes) + ", resident " +
                  mebibytes(process.working_set_bytes) + "; threads " +
                  std::to_string(process.threads));
  if (diagnostics.linux_extension) {
    const auto &x11 = diagnostics.linux_extension->x11;
    lines.push_back("");
    lines.push_back(x11.connected ? "X11: connected"
                                  : "X11: unavailable" + (x11.reason.empty() ? "" : " (" + x11.reason + ")"));
    if (x11.connected) {
      lines.push_back("  " + extension("XI2", x11.xi2) + "; " + extension("Shape", x11.shape) +
                      "; " + extension("RandR", x11.randr));
      lines.push_back("  " + extension("Composite", x11.composite) + "; " +
                      extension("Damage", x11.damage) + "; " + extension("XFixes", x11.xfixes) +
                      "; EWMH " + (x11.ewmh ? "available" : "unavailable"));
    }
    lines.push_back("Capabilities:");
    for (const auto &capability : diagnostics.linux_extension->capabilities) {
      std::string line = "  " + std::string(runtime::capability_name(capability.capability)) +
                         ": " + std::string(runtime::availability_name(capability.availability));
      std::string reasons;
      for (const auto *list : {&capability.reasons, &capability.diagnostics})
        for (const auto &reason : *list) reasons += (reasons.empty() ? "" : "; ") + reason;
      lines.push_back(reasons.empty() ? line : line + " (" + reasons + ")");
    }
  }
  lines.push_back("");
  lines.push_back("Services:");
  if (diagnostics.services.empty()) lines.push_back("  None registered");
  for (const auto &service : diagnostics.services) {
    std::string line = "  " + std::string(runtime::capability_name(service.capability)) + ": " +
                       std::string(runtime::service_state_name(service.state)) + ", " +
                       std::to_string(service.consumers) + " consumer" +
                       (service.consumers == 1U ? "" : "s") + ", " +
                       std::to_string(service.fault_count) + " fault" +
                       (service.fault_count == 1U ? "" : "s");
    if (!service.last_error.empty()) line += "; last error: " + service.last_error;
    lines.push_back(std::move(line));
  }
  lines.push_back("Sessions:");
  if (diagnostics.sessions.empty()) lines.push_back("  None");
  for (const auto &session : diagnostics.sessions) {
    std::string line = "  " + session.profile_id + " on window " + hex(session.window) +
                       (session.foreground ? ", foreground" : ", background") + ", " +
                       std::to_string(session.overlay_windows) + " overlay window" +
                       (session.overlay_windows == 1U ? "" : "s");
    if (!session.last_input_match.empty()) line += "; last input: " + session.last_input_match;
    lines.push_back(std::move(line));
  }
  lines.push_back("Automation faults:");
  if (diagnostics.automation_faults.empty()) lines.push_back("  None");
  for (const auto &fault : diagnostics.automation_faults) lines.push_back("  " + fault);
}
} // namespace

RuntimeDiagnosticsView runtime_diagnostics_view(const std::string_view source,
                                                const bool connected,
                                                const bool authenticated,
                                                const bool detailed) {
  RuntimeDiagnosticsView view{};
  view.lines.push_back(std::string("Connection: ") +
                       (!connected ? "LayerD disconnected"
                        : !authenticated ? "LayerD connected; authentication pending"
                                         : "LayerD connected and authenticated"));
  if (source.empty()) {
    const char *state = connected && authenticated ? "Waiting for snapshot" : "Unavailable";
    view.lines.push_back(std::string("Active profile: ") + state);
    view.lines.push_back(std::string("Isolated visual worker: ") + state);
    view.lines.push_back(connected && authenticated
                             ? "Last error: No diagnostics snapshot received"
                             : "Last error: LayerD is unavailable");
    return view;
  }

  const auto parsed = runtime::parse_runtime_diagnostics_json(source);
  const auto *diagnostics = std::get_if<runtime::RuntimeDiagnostics>(&parsed);
  if (diagnostics == nullptr) {
    view.lines.push_back("Active profile: Unavailable");
    view.lines.push_back("Isolated visual worker: Unavailable");
    view.lines.push_back("Last error: Invalid diagnostics snapshot (" +
                         std::get<core::json::FieldError>(parsed).describe() + ")");
    return view;
  }

  std::string profile_id;
  for (const auto &session : diagnostics->sessions) {
    if (profile_id.empty() || session.foreground) profile_id = session.profile_id;
    if (session.foreground) break;
  }
  view.lines.push_back("Active profile: " + (profile_id.empty() ? std::string("None") : profile_id));

  const auto helper_count = diagnostics->helpers.size();
  const auto running_count = static_cast<std::size_t>(std::ranges::count(
      diagnostics->helpers, true, &runtime::HelperDiagnostics::running));
  const auto quarantined_count = static_cast<std::size_t>(std::ranges::count(
      diagnostics->helpers, true, &runtime::HelperDiagnostics::quarantined));
  view.retry_visual = quarantined_count != 0U;
  std::string worker = "Isolated visual worker: ";
  if (helper_count == 0U)
    worker += "Not active";
  else if (quarantined_count != 0U)
    worker += "Quarantined (" + std::to_string(quarantined_count) + " of " +
              std::to_string(helper_count) + ")";
  else if (running_count == helper_count)
    worker += "Running (" + std::to_string(helper_count) + ")";
  else if (running_count == 0U)
    worker += "Stopped (" + std::to_string(helper_count) + ")";
  else
    worker += "Degraded (" + std::to_string(running_count) + " of " +
              std::to_string(helper_count) + " running)";
  view.lines.push_back(std::move(worker));

  std::string last_error = first_last_error(diagnostics->helpers);
  if (last_error.empty()) last_error = first_last_error(diagnostics->services);
  if (last_error.empty()) last_error = diagnostics->gamebar.last_error;
  if (last_error.empty()) last_error = diagnostics->settings.last_error;
  view.lines.push_back("Last error: " + (last_error.empty() ? std::string("None reported") : last_error));
  if (detailed) append_details(view.lines, *diagnostics);
  return view;
}

} // namespace layerc::editor
