#pragma once

#include "display_types.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct wl_output;

namespace layerc::platform {

class WaylandConnection;

// wlr-layer-shell overlay panels: one `overlay`-layer surface per panel,
// anchored top-left on its output, never taking keyboard focus, with an empty
// input region set before the first commit so it can never take a click.
// Buffers are double-buffered wl_shm XRGB8888 painted by cairo.
class WaylandOverlay {
public:
  WaylandOverlay();
  ~WaylandOverlay();
  WaylandOverlay(const WaylandOverlay &) = delete;
  WaylandOverlay &operator=(const WaylandOverlay &) = delete;

  void sync(const WaylandConnection &connection);
  void release() noexcept;
  // True once after a configure/closed event changed what is on screen.
  [[nodiscard]] bool take_changed() noexcept;
  // Empty when panels can be shown; otherwise stable reasons.
  [[nodiscard]] std::vector<std::string> unavailable() const;

  // Shows or repaints panel `key`. `list` is in global logical pixels on
  // `monitor`, whose origin gives the margins and whose buffer scale the
  // painter uses. A different output recreates the surface. Unchanged
  // content is not repainted. False when the surface cannot exist.
  [[nodiscard]] bool present(std::string_view key, const OverlayDisplayList &list,
                             wl_output *output, const Monitor &monitor);
  // Paints surfaces whose configure arrived since the last present.
  void repaint();
  void remove(std::string_view key) noexcept;
  // Destroys every surface whose key is not listed.
  void retain(std::span<const std::string> keys) noexcept;
  void clear() noexcept;

  [[nodiscard]] std::size_t surface_count() const noexcept;
  // Configured with a committed buffer, i.e. visible to the compositor.
  [[nodiscard]] bool mapped(std::string_view key) const noexcept;
  // Last failure (memfd/mmap/protocol) for status; empty when healthy.
  [[nodiscard]] const std::string &fault() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Paints a display list into an XRGB8888 buffer of `width` x `height` device
// pixels at integer `scale` (ops are in logical panel-local pixels). Pure, so
// the painter is verifiable without a compositor.
void paint_overlay_display_list(const OverlayDisplayList &list, unsigned char *pixels,
                                int width, int height, int stride, int scale);

} // namespace layerc::platform
