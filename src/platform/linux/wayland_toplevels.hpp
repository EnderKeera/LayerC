#pragma once

#include "display_types.hpp"

#include <memory>
#include <vector>

struct wl_output;

namespace layerc::platform {

class WaylandConnection;
class WaylandMonitors;

// Read-only wlr-foreign-toplevel-management window tracking. No activate,
// close or other mutating request is ever sent. The protocol has neither
// PIDs nor window geometry: executable matching is unavailable and geometry
// is the rect of the first output a toplevel entered, or of the only monitor
// when the compositor sends no output_enter; otherwise it is empty.
class WaylandToplevels {
public:
  WaylandToplevels();
  ~WaylandToplevels();
  WaylandToplevels(const WaylandToplevels &) = delete;
  WaylandToplevels &operator=(const WaylandToplevels &) = delete;

  void sync(const WaylandConnection &connection);
  void release() noexcept;
  // Drops destroyed output proxies from every toplevel's entered outputs.
  void forget_outputs(const std::vector<const wl_output *> &outputs);
  [[nodiscard]] bool take_changed() noexcept;
  // True while the toplevel manager is bound.
  [[nodiscard]] bool available() const noexcept;
  // Toplevels in creation order, capped at window_scan_limit. Ids are opaque,
  // nonzero and stable while the toplevel lives.
  [[nodiscard]] WindowScan scan(const WaylandMonitors &monitors) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::platform
