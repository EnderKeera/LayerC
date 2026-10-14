#include "display_selection.hpp"

namespace layerc::daemon {
namespace {
bool contains_ascii(std::string_view text, std::string_view token) noexcept {
  const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; };
  for (std::size_t pos = 0; pos + token.size() <= text.size(); ++pos) {
    bool equal = true;
    for (std::size_t i = 0; i < token.size(); ++i)
      equal = equal && upper(text[pos + i]) == upper(token[i]);
    if (equal) return true;
  }
  return false;
}
} // namespace

layerc::runtime::DisplayBackend display_backend(std::string_view value) noexcept {
  using enum layerc::runtime::DisplayBackend;
  if (value.empty() || value == "auto") return auto_detect;
  if (value == "x11") return x11;
  if (value == "wayland") return wayland;
  return invalid;
}

layerc::runtime::DisplaySession select_display_session(
    layerc::runtime::DisplayBackend backend, bool wayland, bool x11) noexcept {
  using layerc::runtime::DisplayBackend;
  using layerc::runtime::DisplaySession;
  if (backend == DisplayBackend::invalid) return DisplaySession::none;
  if (backend != DisplayBackend::x11 && wayland) return DisplaySession::wayland;
  if (backend != DisplayBackend::wayland && x11) return DisplaySession::x11;
  return DisplaySession::none;
}

layerc::runtime::DisplayCompositor detect_compositor(
    std::string_view desktop, bool kwin_owner, bool gnome_owner,
    bool layer_shell, bool foreign_toplevel) noexcept {
  using enum layerc::runtime::DisplayCompositor;
  if (kwin_owner || contains_ascii(desktop, "KDE")) return kwin;
  if (gnome_owner || contains_ascii(desktop, "GNOME")) return gnome;
  if (layer_shell && foreign_toplevel) return wlroots;
  return unknown;
}
} // namespace layerc::daemon
