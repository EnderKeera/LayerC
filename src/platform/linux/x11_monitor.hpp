#pragma once

#include "x11_display.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace layerc::platform {

// Panel geometry is authored at 96 dpi (Windows MulDiv parity), so every X11
// size is converted before it reaches the server.
inline constexpr int baseline_dpi = 96;
// Physical monitor sizes in EDID are frequently 0 or absurd; keep the derived
// scale inside a range a UI can survive.
inline constexpr int min_dpi = 72;
inline constexpr int max_dpi = 288;

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

// Mean of the horizontal and vertical DPI, clamped; unknown physical size
// (0 mm) yields the baseline.
[[nodiscard]] int dpi_from_geometry(int width, int height, int mm_width,
                                   int mm_height) noexcept;

// Largest-overlap monitor wins; the primary, then the first monitor, break
// ties. An empty scan yields the baseline.
[[nodiscard]] int monitor_dpi_for_rect(const X11MonitorScan &scan, int x, int y,
                                      int width, int height) noexcept;

[[nodiscard]] int scaled_pixels(int value, int dpi) noexcept;

} // namespace layerc::platform
