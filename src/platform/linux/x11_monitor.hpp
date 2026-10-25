#pragma once

#include "display_types.hpp"
#include "x11_display.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace layerc::platform {

// The 96 dpi baseline, its clamp range, dpi_from_geometry and scaled_pixels
// are the display-server-neutral helpers in display_types.hpp.

// One RandR output per monitor. X11 has no per-monitor DPI convention, so the
// server's physical size is the only source for the scale factor, and
// `available` means RandR answered.
using X11Monitor = Monitor;
using X11MonitorScan = MonitorScan;

[[nodiscard]] X11MonitorScan scan_monitors(const X11Display &display);

} // namespace layerc::platform
