// RandR monitor geometry + the scale panel sizes need (V2.6.1). Minimal by
// design: only what the existing panel geometry consumes, no mode setting.
#include "x11_monitor.hpp"
#include "x11_error.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <utility>

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
  scan.available = true;
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
