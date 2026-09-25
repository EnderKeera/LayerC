// Click-through overlay windows (V2.6.1): one override-redirect window per
// panel, shaped to an empty input region so it can never intercept a click.
// Rectangles only — no compositing, no toolkit, no input synthesis.
#include "x11_overlay.hpp"
#include "x11_error.hpp"
#include "layerc/core/panel_geometry.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace layerc::platform {
namespace {

[[nodiscard]] unsigned long fit_channel(const unsigned channel,
                                        const unsigned long mask) noexcept {
  if (mask == 0) return 0;
  unsigned shift = 0;
  while (((mask >> shift) & 1U) == 0) ++shift;
  const unsigned long bits = mask >> shift;
  return ((static_cast<unsigned long>(channel) * bits + 127U) / 255U)
         << shift;
}

} // namespace

std::vector<std::string> overlay_unavailable(const X11Probe &probe) {
  std::vector<std::string> reasons;
  if (!probe.connected)
    reasons.push_back("overlay: no X display available");
  else if (!probe.shape.present)
    reasons.push_back(
        "overlay: shape extension not advertised by the X server");
  return reasons;
}

X11OverlayRect panel_rect(const X11OverlayFrame &frame) {
  const int dpi = frame.dpi;
  const auto geometry =
      layerc::core::panel_geometry(frame.panel, frame.bars.size());
  const int width = std::max(scaled_pixels(geometry.size.width, dpi), 1);
  const int height = std::max(scaled_pixels(geometry.size.height, dpi), 1);
  X11OverlayRect rect;
  rect.width = width;
  rect.height = height;
  rect.x = frame.target.x +
           static_cast<int>(
               std::lround(frame.panel.anchor_x *
                           static_cast<double>(frame.target.width))) -
           width / 2;
  rect.y = frame.target.y +
           static_cast<int>(
               std::lround(frame.panel.anchor_y *
                           static_cast<double>(frame.target.height))) -
           height / 2;
  return rect;
}

X11OverlayWindow::X11OverlayWindow(const X11Display &display,
                                   const std::uint64_t window) noexcept
    : display_(&display), window_(window) {}

X11OverlayWindow::X11OverlayWindow(X11OverlayWindow &&other) noexcept
    : display_(std::exchange(other.display_, nullptr)),
      window_(std::exchange(other.window_, 0)),
      gc_(std::exchange(other.gc_, nullptr)),
      font_set_(std::exchange(other.font_set_, nullptr)),
      font_set_tried_(other.font_set_tried_), rect_(other.rect_),
      visible_(std::exchange(other.visible_, false)),
      truecolor_(std::exchange(other.truecolor_, false)),
      red_mask_(other.red_mask_), green_mask_(other.green_mask_),
      blue_mask_(other.blue_mask_),
      colormap_pixels_(std::move(other.colormap_pixels_)) {}

X11OverlayWindow &X11OverlayWindow::operator=(X11OverlayWindow &&other) noexcept {
  if (this != &other) {
    destroy();
    display_ = std::exchange(other.display_, nullptr);
    window_ = std::exchange(other.window_, 0);
    gc_ = std::exchange(other.gc_, nullptr);
    font_set_ = std::exchange(other.font_set_, nullptr);
    font_set_tried_ = other.font_set_tried_;
    rect_ = other.rect_;
    visible_ = std::exchange(other.visible_, false);
    truecolor_ = std::exchange(other.truecolor_, false);
    red_mask_ = other.red_mask_;
    green_mask_ = other.green_mask_;
    blue_mask_ = other.blue_mask_;
    colormap_pixels_ = std::move(other.colormap_pixels_);
  }
  return *this;
}

X11OverlayWindow::~X11OverlayWindow() { destroy(); }

void X11OverlayWindow::destroy() noexcept {
  if (display_ == nullptr || window_ == 0) return;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  const auto window = static_cast<::Window>(window_);
  if (raw != nullptr) {
    if (font_set_ != nullptr) XFreeFontSet(raw, font_set_);
    if (gc_ != nullptr) XFreeGC(raw, gc_);
    X11ErrorTrap trap{raw};
    if (!colormap_pixels_.empty()) {
      const auto allocated = colormap_pixels_ | std::views::values;
      std::vector<unsigned long> pixels(allocated.begin(), allocated.end());
      XFreeColors(raw, DefaultColormap(raw, DefaultScreen(raw)),
                  pixels.data(), static_cast<int>(pixels.size()), 0);
    }
    XDestroyWindow(raw, window);
    (void)trap.ok();
  }
  font_set_ = nullptr;
  gc_ = nullptr;
  window_ = 0;
  visible_ = false;
  colormap_pixels_.clear();
}

std::optional<X11OverlayWindow>
X11OverlayWindow::create(const X11Display &display, std::string &error) {
  error.clear();
  auto *raw = reinterpret_cast<Display *>(display.raw());
  if (raw == nullptr) {
    error = "overlay: no X display";
    return std::nullopt;
  }
  if (const auto reasons = overlay_unavailable(display.probe());
      !reasons.empty()) {
    error = reasons.front();
    return std::nullopt;
  }

  X11ErrorTrap trap{raw};
  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  attributes.background_pixel = BlackPixel(raw, DefaultScreen(raw));
  attributes.border_pixel = 0;
  // The daemon repaints on its own deadline, so the window asks for no events.
  attributes.event_mask = 0;
  const ::Window window = XCreateWindow(
      raw, static_cast<::Window>(display.root()), 0, 0, 1, 1, 0, CopyFromParent,
      InputOutput, CopyFromParent,
      CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
      &attributes);
  if (!trap.ok()) {
    error = "overlay: XCreateWindow failed";
    return std::nullopt;
  }

  X11OverlayWindow result{display, static_cast<std::uint64_t>(window)};
  GC gc = XCreateGC(raw, window, 0, nullptr);
  if (gc == nullptr) {
    error = "overlay: XCreateGC failed";
    return std::nullopt;
  }
  result.gc_ = gc;

  // Resolve the pixel path once: the window inherits the root visual
  // (CopyFromParent), so a TrueColor default visual paints arithmetically
  // with no server round-trip, while colormap visuals share one cache
  // across paints until destroy() frees it.
  const int screen = DefaultScreen(raw);
  XVisualInfo matched{};
  if (XMatchVisualInfo(raw, screen, DefaultDepth(raw, screen), TrueColor,
                       &matched) != 0) {
    result.truecolor_ = true;
    result.red_mask_ = matched.red_mask;
    result.green_mask_ = matched.green_mask;
    result.blue_mask_ = matched.blue_mask;
  }

  // The whole point of the Shape requirement: no input region at all, so a
  // click passes through to the window below.
  XShapeCombineRectangles(raw, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet,
                          YXBanded);
  XSync(raw, False);
  if (!trap.ok()) {
    error = "overlay: XShapeCombineRectangles failed";
    return std::nullopt;
  }
  // Verified, not assumed: an overlay that could swallow a click is never
  // shown, so a server that ignores the request leaves the window hidden.
  if (const int count = result.input_rectangle_count(); count != 0) {
    error = "overlay: input shape is not empty (" + std::to_string(count) +
            " rectangle(s))";
    return std::nullopt;
  }
  return result;
}

int X11OverlayWindow::input_rectangle_count() const {
  if (display_ == nullptr || window_ == 0) return -1;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  if (raw == nullptr) return -1;
  int count = -1;
  int order{};
  X11ErrorTrap trap{raw};
  XRectangle *rectangles =
      XShapeGetRectangles(raw, static_cast<::Window>(window_), ShapeInput,
                          &count, &order);
  if (rectangles != nullptr) XFree(rectangles);
  if (!trap.ok()) return -1;
  return count;
}

unsigned long X11OverlayWindow::resolve_pixel(const std::uint32_t rgb) {
  if (truecolor_) {
    return fit_channel((rgb >> 16U) & 0xFFU, red_mask_) |
           fit_channel((rgb >> 8U) & 0xFFU, green_mask_) |
           fit_channel(rgb & 0xFFU, blue_mask_);
  }
  if (const auto found = colormap_pixels_.find(rgb);
      found != colormap_pixels_.end())
    return found->second;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  const int screen = DefaultScreen(raw);
  XColor color{};
  color.red = static_cast<unsigned short>(((rgb >> 16U) & 0xFFU) * 257);
  color.green = static_cast<unsigned short>(((rgb >> 8U) & 0xFFU) * 257);
  color.blue = static_cast<unsigned short>((rgb & 0xFFU) * 257);
  color.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(raw, DefaultColormap(raw, screen), &color) == 0)
    return BlackPixel(raw, screen);
  colormap_pixels_.emplace(rgb, color.pixel);
  return color.pixel;
}

bool X11OverlayWindow::update(const X11OverlayFrame &frame) {
  if (display_ == nullptr || window_ == 0 || gc_ == nullptr) return false;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  if (raw == nullptr) return false;
  const auto window = static_cast<::Window>(window_);
  const X11OverlayRect next = panel_rect(frame);
  if (next.width <= 0 || next.height <= 0) {
    hide();
    return false;
  }

  X11ErrorTrap trap{raw};
  if (next != rect_) {
    XMoveResizeWindow(raw, window, next.x, next.y,
                      static_cast<unsigned int>(next.width),
                      static_cast<unsigned int>(next.height));
    rect_ = next;
  }
  if (!visible_) {
    XMapRaised(raw, window);
    visible_ = true;
  }

  const auto *panel = &frame.panel;
  const bool bar_style = panel->style == layerc::core::PanelStyle::bar;
  const auto background = resolve_pixel(bar_style ? panel->bar.background_color
                                                  : 0x272D35U);
  const auto border =
      resolve_pixel(bar_style ? panel->bar.border_color : 0x50BAC8U);
  const auto fill =
      resolve_pixel(bar_style ? panel->bar.fill_color : 0x50BAC8U);
  const auto text_color =
      resolve_pixel(bar_style ? panel->bar.text_color : 0xE4E8ECU);

  XSetForeground(raw, gc_, background);
  XFillRectangle(raw, window, gc_, 0, 0, static_cast<unsigned int>(next.width),
                 static_cast<unsigned int>(next.height));
  XSetForeground(raw, gc_, border);
  XDrawRectangle(raw, window, gc_, 0, 0,
                 static_cast<unsigned int>(next.width - 1),
                 static_cast<unsigned int>(next.height - 1));

  const auto geometry =
      layerc::core::panel_geometry(*panel, frame.bars.size());
  XFontSet font_set = nullptr;
  if (!bar_style || panel->bar.show_text) {
    if (font_set_ == nullptr && !font_set_tried_) {
      font_set_tried_ = true;
      char **missing{};
      int missing_count{};
      char *default_string{};
      font_set_ = XCreateFontSet(raw, "fixed", &missing, &missing_count,
                                 &default_string);
      if (missing != nullptr) XFreeStringList(missing);
    }
    font_set = font_set_;
  }

  for (std::size_t index = 0; index < frame.bars.size(); ++index) {
    const auto &bar = frame.bars[index];
    auto item = geometry.items[index];
    if (bar_style) {
      const auto shape = layerc::core::bar_geometry(
          *panel, index, frame.bars.size(), bar.remaining);
      item = shape.outline;
      if (shape.fill.right > shape.fill.left &&
          shape.fill.bottom > shape.fill.top) {
        XSetForeground(raw, gc_, fill);
        XFillRectangle(
            raw, window, gc_, scaled_pixels(shape.fill.left, frame.dpi),
            scaled_pixels(shape.fill.top, frame.dpi),
            static_cast<unsigned int>(
                std::max(1, scaled_pixels(shape.fill.width(), frame.dpi))),
            static_cast<unsigned int>(
                std::max(1, scaled_pixels(shape.fill.height(), frame.dpi))));
      }
    } else {
      XSetForeground(raw, gc_, border);
      XDrawRectangle(
          raw, window, gc_, scaled_pixels(item.left, frame.dpi),
          scaled_pixels(item.top, frame.dpi),
          static_cast<unsigned int>(
              std::max(1, scaled_pixels(item.width(), frame.dpi) - 1)),
          static_cast<unsigned int>(
              std::max(1, scaled_pixels(item.height(), frame.dpi) - 1)));
    }

    if (font_set == nullptr || bar.label.empty()) continue;
    XRectangle ink{};
    XRectangle logical{};
    Xutf8TextExtents(font_set, bar.label.data(),
                     static_cast<int>(bar.label.size()), &ink, &logical);
    const int left = scaled_pixels(item.left, frame.dpi);
    const int top = scaled_pixels(item.top, frame.dpi);
    const int item_width = scaled_pixels(item.width(), frame.dpi);
    const int item_height = scaled_pixels(item.height(), frame.dpi);
    const int text_x = left + (item_width - logical.width) / 2;
    const int text_y = top + (item_height - logical.height) / 2 - logical.y;
    XSetForeground(raw, gc_, text_color);
    Xutf8DrawString(raw, window, font_set, gc_, text_x, text_y,
                    bar.label.data(), static_cast<int>(bar.label.size()));
  }

  // Nothing in the repaint path waits for a reply, so the buffered drawing
  // commands have to be flushed explicitly.
  XFlush(raw);
  return trap.ok();
}

void X11OverlayWindow::raise() noexcept {
  if (display_ == nullptr || window_ == 0 || !visible_) return;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  if (raw == nullptr) return;
  X11ErrorTrap trap{raw};
  XRaiseWindow(raw, static_cast<::Window>(window_));
  (void)trap.ok();
}

void X11OverlayWindow::hide() noexcept {
  if (display_ == nullptr || window_ == 0 || !visible_) return;
  auto *raw = reinterpret_cast<Display *>(display_->raw());
  if (raw == nullptr) return;
  X11ErrorTrap trap{raw};
  XUnmapWindow(raw, static_cast<::Window>(window_));
  (void)trap.ok();
  visible_ = false;
}

} // namespace layerc::platform
