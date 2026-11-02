#include "capture_route.hpp"

namespace layerc::daemon {
namespace {

[[nodiscard]] std::string creation_failure(const std::uint64_t window) {
  return "visual: capture could not be created for window " +
         std::to_string(window);
}

} // namespace

std::optional<std::uint64_t>
capture_x11_identity(const CaptureRoute &route, const std::uint64_t window) {
  if (!route.native_windows) return window;
  if (route.native == nullptr) return std::nullopt;
  return route.native->x11_window(window);
}

CaptureResolution resolve_capture(const CaptureRoute &route,
                                  const std::string_view profile_id,
                                  const std::uint64_t window,
                                  const layerc::core::CaptureQuality quality,
                                  const std::uint8_t sample_rate) {
  CaptureResolution resolution;
  const auto create = [&](CaptureFactory *factory,
                          const std::uint64_t identity) {
    resolution.session =
        factory->create_capture(profile_id, {identity}, quality, sample_rate);
    if (!resolution.session)
      resolution.unavailable = creation_failure(window);
  };
  const auto identity = capture_x11_identity(route, window);
  if (route.native_windows) {
    if (route.native == nullptr) {
      // A compositor identity is not an X11 window id; there is nothing to
      // capture and nothing to ask consent for.
      resolution.unavailable = "visual: native windows have no X11 window id";
      return resolution;
    }
    if (identity) {
      // A reported XWayland window is an X11 target: portal consent would
      // capture a different source, so it is never a fallback here.
      resolution.x11_window = *identity;
      if (route.x11 == nullptr) {
        resolution.unavailable = "visual: window " + std::to_string(window) +
                                 " is an XWayland window but X11 capture is "
                                 "unavailable";
        return resolution;
      }
      create(route.x11, *identity);
      return resolution;
    }
    create(route.native, window);
    return resolution;
  }
  if (route.x11 == nullptr) {
    resolution.unavailable = "visual: no X display for capture";
    return resolution;
  }
  resolution.x11_window = *identity;
  create(route.x11, *identity);
  return resolution;
}

} // namespace layerc::daemon
