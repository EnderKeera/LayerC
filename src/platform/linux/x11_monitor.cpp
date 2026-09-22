// RandR monitor geometry + the scale panel sizes need (V2.6.1). Minimal by
// design: only what the existing panel geometry consumes, no mode setting.
#include "x11_monitor.hpp"
#include "x11_error.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cmath>

namespace layerc::platform {
namespace {

// Some servers (Xvfb among them) report 0 mm per output while the screen as a
// whole carries a plausible physical size; scale it by the output's share.
[[nodiscard]] int derived_mm(int output_pixels, int screen_pixels, int screen_mm,
                             int fallback_mm) noexcept {
  if (fallback_mm > 0) return fallback_mm;
  if (screen_pixels <= 0 || screen_mm <= 0) return 0;
  return screen_mm * output_pixels / screen_pixels;
}

} // namespace

int dpi_from_geometry(const int width, const int height, const int mm_width,
                      const int mm_height) noexcept {
  if (width <= 0 || height <= 0 || mm_width <= 0 || mm_height <= 0)
    return baseline_dpi;
  const double horizontal =
      static_cast<double>(width) * 25.4 / static_cast<double>(mm_width);
  const double vertical =
      static_cast<double>(height) * 25.4 / static_cast<double>(mm_height);
  const int dpi = static_cast<int>(std::lround((horizontal + vertical) / 2.0));
  return std::clamp(dpi, min_dpi, max_dpi);
}

int monitor_dpi_for_rect(const X11MonitorScan &scan, const int x, const int y,
                         const int width, const int height) noexcept {
  if (scan.monitors.empty()) return baseline_dpi;
  const X11Monitor *best = nullptr;
  std::int64_t best_overlap = 0;
  for (const auto &monitor : scan.monitors) {
    const int left = std::max(x, monitor.x);
    const int right = std::min(x + width, monitor.x + monitor.width);
    const int top = std::max(y, monitor.y);
    const int bottom = std::min(y + height, monitor.y + monitor.height);
    const auto overlap = static_cast<std::int64_t>(std::max(0, right - left)) *
                         static_cast<std::int64_t>(std::max(0, bottom - top));
    if (best == nullptr || overlap > best_overlap) {
      best = &monitor;
      best_overlap = overlap;
    }
  }
  if (best_overlap > 0 && best != nullptr) return best->dpi;
  for (const auto &monitor : scan.monitors)
    if (monitor.primary) return monitor.dpi;
  return scan.monitors.front().dpi;
}

int scaled_pixels(const int value, const int dpi) noexcept {
  return (value * dpi + baseline_dpi / 2) / baseline_dpi;
}

X11MonitorScan scan_monitors(const X11Display &display) {
  X11MonitorScan scan;
  auto *raw = reinterpret_cast<Display *>(display.raw());
  if (raw == nullptr) {
    scan.warning = "monitor: no X display for the monitor scan";
    return scan;
  }
  if (!display.probe().randr.present) {
    scan.warning = "randr: extension not advertised by the X server";
    return scan;
  }

  const int screen = DefaultScreen(raw);
  const int screen_width = DisplayWidth(raw, screen);
  const int screen_height = DisplayHeight(raw, screen);
  const int screen_mm_width = DisplayWidthMM(raw, screen);
  const int screen_mm_height = DisplayHeightMM(raw, screen);
  const auto root = static_cast<::Window>(display.root());

  X11ErrorTrap trap{raw};
  XRRScreenResources *resources = XRRGetScreenResources(raw, root);
  if (resources == nullptr || !trap.ok()) {
    if (resources != nullptr) XRRFreeScreenResources(resources);
    scan.warning = "randr: XRRGetScreenResources failed";
    return scan;
  }
  scan.randr = true;
  // The primary output needs RandR 1.3; without it the first monitor wins.
  const RROutput primary =
      display.probe().randr.major > 1 || display.probe().randr.minor >= 3
          ? XRRGetOutputPrimary(raw, root)
          : None;

  for (int index = 0; index < resources->noutput; ++index) {
    const RROutput output = resources->outputs[index];
    XRROutputInfo *info = XRRGetOutputInfo(raw, resources, output);
    if (info == nullptr) continue;
    const bool connected = info->connection == RR_Connected && info->crtc != None;
    const auto mm_width = static_cast<int>(info->mm_width);
    const auto mm_height = static_cast<int>(info->mm_height);
    std::string name = info->name != nullptr ? info->name : "";
    const RRCrtc crtc = info->crtc;
    XRRFreeOutputInfo(info);
    if (!connected) continue;

    XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(raw, resources, crtc);
    if (crtc_info == nullptr) continue;
    X11Monitor monitor;
    monitor.name = std::move(name);
    monitor.x = crtc_info->x;
    monitor.y = crtc_info->y;
    monitor.width = static_cast<int>(crtc_info->width);
    monitor.height = static_cast<int>(crtc_info->height);
    monitor.primary = output == primary;
    XRRFreeCrtcInfo(crtc_info);
    monitor.dpi = dpi_from_geometry(
        monitor.width, monitor.height,
        derived_mm(monitor.width, screen_width, screen_mm_width, mm_width),
        derived_mm(monitor.height, screen_height, screen_mm_height, mm_height));
    scan.monitors.push_back(std::move(monitor));
  }
  XRRFreeScreenResources(resources);
  if (!trap.ok()) scan.warning = "randr: a monitor query failed";
  return scan;
}

} // namespace layerc::platform
