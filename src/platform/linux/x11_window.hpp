#pragma once

#include "display_types.hpp"
#include "x11_display.hpp"

#include "layerc/core/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace layerc::platform {

// The tracker reports the display-server-neutral shapes; `authoritative`
// means the scan came from the EWMH client list.
using X11WindowSnapshot = WindowSnapshot;
using X11WindowScan = WindowScan;

class X11WindowTracker {
public:
  explicit X11WindowTracker(const X11Display &display) noexcept;
  X11WindowTracker(const X11WindowTracker &) = delete;
  X11WindowTracker &operator=(const X11WindowTracker &) = delete;
  ~X11WindowTracker();

  // Bounded scan: at most `limit` mapped top-level windows, root-relative.
  [[nodiscard]] X11WindowScan scan(std::uint32_t limit = 64) const;

  // Discovery observes the same dispatcher as input: client identity/state,
  // root properties, and map/unmap/destroy/configure events invalidate scans.
  // The daemon's own overlay windows: their map/restack/destroy never
  // invalidate scans, so raising an overlay cannot trigger another refresh.
  void ignore_windows(std::vector<std::uint64_t> windows) noexcept;
  [[nodiscard]] bool observe(const union _XEvent &event) const noexcept;
  // Uses the shared dispatcher while input is inactive, without polling timers.
  [[nodiscard]] bool poll_changes() noexcept;

  [[nodiscard]] std::optional<X11WindowSnapshot> find(std::uint64_t window) const;

private:
  struct Watch {
    std::uint64_t window{};
    long previous_mask{};
  };
  void update_watches(const std::vector<std::uint64_t> &windows) const;
  const X11Display *display_{};
  std::array<std::uint64_t, 5> identity_atoms_{};
  long root_previous_mask_{};
  bool root_selected_{};
  mutable std::vector<Watch> watched_;
  std::vector<std::uint64_t> ignored_windows_;
};


} // namespace layerc::platform
