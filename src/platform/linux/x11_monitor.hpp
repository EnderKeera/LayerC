#pragma once

#include "display_types.hpp"
#include "x11_display.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace layerc::platform {

// The 96 dpi baseline, its clamp range, dpi_from_geometry and scaled_pixels
// are the display-server-neutral helpers in display_types.hpp.

// One RandR output. X11 has no per-monitor DPI convention, so the server's
// physical size is the only source for the scale factor.
struct X11Monitor {
  std::string name;
  int x{};
  int y{};
  int width{};
  int height{};
  bool primary{};
  int dpi{baseline_dpi};
};

struct X11MonitorScan {
  bool randr{};
  std::vector<X11Monitor> monitors;
  // Set when RandR is missing or a query failed: the caller publishes it and
  // geometry falls back to the 96 dpi baseline.
  std::string warning;
};

[[nodiscard]] X11MonitorScan scan_monitors(const X11Display &display);

// Largest-overlap monitor wins; the primary, then the first monitor, break
// ties. An empty scan yields the baseline.
[[nodiscard]] int monitor_dpi_for_rect(const X11MonitorScan &scan, int x, int y,
                                      int width, int height) noexcept;

} // namespace layerc::platform
