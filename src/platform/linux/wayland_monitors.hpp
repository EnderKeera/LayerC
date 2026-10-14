#pragma once

#include "display_types.hpp"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

struct wl_output;

namespace layerc::platform {

class WaylandConnection;

// wl_output plus zxdg_output_manager_v1 monitors in global logical pixels.
// Wayland has no per-monitor DPI convention for clients: monitors report the
// 96 dpi baseline and an integer buffer scale for crisp client rendering.
class WaylandMonitors {
public:
  WaylandMonitors();
  ~WaylandMonitors();
  WaylandMonitors(const WaylandMonitors &) = delete;
  WaylandMonitors &operator=(const WaylandMonitors &) = delete;

  // Binds newly advertised outputs/manager and releases removed ones.
  void sync(const WaylandConnection &connection);
  // Destroys every proxy; the owning connection is about to disconnect.
  void release() noexcept;
  // True once after any output, geometry or scale change.
  [[nodiscard]] bool take_changed() noexcept;
  // Output proxies destroyed since the last call (identity only; never
  // dereference them).
  [[nodiscard]] std::vector<const wl_output *> take_removed();
  [[nodiscard]] MonitorScan scan() const;
  // The output proxy of a scanned monitor, for layer surfaces.
  [[nodiscard]] wl_output *output(std::string_view name) const noexcept;
  // The scanned monitor an output proxy belongs to (toplevel geometry).
  [[nodiscard]] std::optional<Monitor> monitor(const wl_output *output) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::platform
