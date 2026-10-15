#pragma once

#include "layerc/runtime/display_diagnostics.hpp"

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

// "wayland session, compositor wlroots, desktop sway": which display server
// and compositor LayerD detected, and any LAYERC_DISPLAY_BACKEND override.
[[nodiscard]] std::string
display_session_text(const layerc::runtime::DisplayStatusSnapshot &display);
// "x11_ewmh: degraded (xwayland_windows)": one provider row with its
// state and stable reason tokens.
[[nodiscard]] std::string
display_provider_text(const layerc::runtime::DisplayProviderStatus &provider);

} // namespace layerc::editor
