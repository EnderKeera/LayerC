#pragma once

#include "x11_display.hpp"
#include "x11_monitor.hpp"

#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Opaque Xlib handles; Xlib's macros must not reach every translation unit.
// XFontSet is a struct _XOC pointer, which is why this is not _XFontSet.
struct _XGC;
struct _XOC;

namespace layerc::platform {

// Overlay windows paint the neutral frame (root coordinates on X11) and are
// placed by the shared panel_rect.
using X11OverlayRect = OverlayRect;
using X11OverlayBar = OverlayBar;
using X11OverlayFrame = OverlayFrame;

// Reasons no overlay may be shown at all. Without Shape the window would
// intercept clicks, so it stays hidden instead.
[[nodiscard]] std::vector<std::string>
overlay_unavailable(const X11Probe &probe);

// One click-through overlay window: override-redirect (no WM decoration or
// focus), always repainted in full, and shaped to an empty input region so it
// can never take a click.
class X11OverlayWindow {
public:
  X11OverlayWindow() = default;
  X11OverlayWindow(const X11OverlayWindow &) = delete;
  X11OverlayWindow &operator=(const X11OverlayWindow &) = delete;
  X11OverlayWindow(X11OverlayWindow &&other) noexcept;
  X11OverlayWindow &operator=(X11OverlayWindow &&other) noexcept;
  ~X11OverlayWindow();

  // Fails when Shape is missing or the empty input region cannot be verified:
  // an overlay that could intercept input is never shown.
  [[nodiscard]] static std::optional<X11OverlayWindow>
  create(const X11Display &display, std::string &error);

  // Moves/resizes to the frame's panel rect, paints it and maps the window.
  // False once the window is gone; the caller drops it.
  [[nodiscard]] bool update(const X11OverlayFrame &frame);
  void raise() noexcept;
  void hide() noexcept;

  [[nodiscard]] bool valid() const noexcept { return window_ != 0; }
  [[nodiscard]] bool visible() const noexcept { return visible_; }
  [[nodiscard]] std::uint64_t window() const noexcept { return window_; }

  // Verification: rectangles in the ShapeInput region. Zero means the window
  // takes no input at all, which is what makes it click-through. -1 when the
  // query failed.
  [[nodiscard]] int input_rectangle_count() const;

private:
  explicit X11OverlayWindow(const X11Display &display,
                            std::uint64_t window) noexcept;
  void destroy() noexcept;
  // Pixel without colormap churn (see x11_overlay.cpp): TrueColor visuals
  // compute arithmetically from the channel masks, colormap visuals share
  // the cache below across paints until destroy() frees it.
  [[nodiscard]] unsigned long resolve_pixel(std::uint32_t rgb);

  const X11Display *display_{};
  std::uint64_t window_{};
  struct _XGC *gc_{};
  struct _XOC *font_set_{};
  bool font_set_tried_{};
  X11OverlayRect rect_{};
  bool visible_{};
  bool truecolor_{};
  unsigned long red_mask_{};
  unsigned long green_mask_{};
  unsigned long blue_mask_{};
  std::unordered_map<std::uint32_t, unsigned long> colormap_pixels_;
};

} // namespace layerc::platform
