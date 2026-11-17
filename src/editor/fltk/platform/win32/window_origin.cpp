#include "a11y/window_geometry.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H>

namespace layerc::editor::a11y {

std::optional<std::pair<int, int>> window_screen_origin(Fl_Window &window) {
  if (!window.shown()) return std::nullopt;
  const auto hwnd = fl_xid(&window);
  POINT origin{};
  if (hwnd == nullptr || !ClientToScreen(hwnd, &origin)) return std::nullopt;
  return std::pair{static_cast<int>(origin.x), static_cast<int>(origin.y)};
}

} // namespace layerc::editor::a11y
