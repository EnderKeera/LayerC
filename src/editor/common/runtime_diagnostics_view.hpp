#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor {

// Toolkit-free text for a Runtime Diagnostics view, built from a daemon's
// RuntimeDiagnostics JSON snapshot. Both frontends show the same summary;
// the detailed form adds process, Linux/X11 capability, service, session and
// automation-fault sections.
struct RuntimeDiagnosticsView {
  std::vector<std::string> lines;
  // An isolated visual worker is quarantined; the daemon can retry it.
  bool retry_visual{};
};

[[nodiscard]] RuntimeDiagnosticsView
runtime_diagnostics_view(std::string_view source, bool connected,
                         bool authenticated, bool detailed);

} // namespace layerc::editor
