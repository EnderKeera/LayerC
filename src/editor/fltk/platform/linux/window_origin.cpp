#include "a11y/window_geometry.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <X11/Xlib.h>

namespace layerc::editor::a11y {

std::optional<std::pair<int, int>> window_screen_origin(Fl_Window &window) {
  if (!window.shown()) return std::nullopt;
  auto *display = fl_x11_display();
  const auto xid = fl_x11_xid(&window);
  int origin_x{}, origin_y{};
  Window child{};
  if (!display || !xid ||
      !XTranslateCoordinates(display, xid, DefaultRootWindow(display), 0, 0, &origin_x,
                             &origin_y, &child))
    return std::nullopt;
  return std::pair{origin_x, origin_y};
}

} // namespace layerc::editor::a11y
