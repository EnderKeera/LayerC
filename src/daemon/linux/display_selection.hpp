#pragma once

#include "layerc/runtime/display_diagnostics.hpp"

namespace layerc::daemon {

// Pure selection policy; the booleans are successful connection probes, not
// environment hints. Forced backends never silently fall back to another.
[[nodiscard]] layerc::runtime::DisplayBackend
display_backend(std::string_view value) noexcept;
[[nodiscard]] layerc::runtime::DisplaySession select_display_session(
    layerc::runtime::DisplayBackend backend, bool wayland, bool x11) noexcept;
[[nodiscard]] layerc::runtime::DisplayCompositor detect_compositor(
    std::string_view desktop, bool kwin_owner, bool gnome_owner,
    bool layer_shell, bool foreign_toplevel) noexcept;

} // namespace layerc::daemon
