#pragma once

// The one capture-routing decision for a resolved Linux display route. Native
// handles, consent and transport lifetimes stay in the providers.
#include "native_capture.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::daemon {

// The capture providers one resolved display route offers. `native_windows`
// records that window identities come from a compositor, not X11.
struct CaptureRoute {
  CaptureFactory *native{};
  CaptureFactory *x11{};
  bool native_windows{};
  bool operator==(const CaptureRoute &) const = default;
};

// One target's capture after routing: an owned session, or the reason it
// cannot be sampled. `x11_window` is the identity a session was created for.
struct CaptureResolution {
  std::unique_ptr<VisualCaptureSession> session;
  std::string unavailable;
  std::optional<std::uint64_t> x11_window;
};

// The X11 identity of one window on this route, or nullopt when the route has
// no X11 identity for it. The only interpretation of a window id: the routing
// owner and the visual service's reuse check both call this.
[[nodiscard]] std::optional<std::uint64_t>
capture_x11_identity(const CaptureRoute &route, std::uint64_t window);

// Routes one window to exactly one capture transport. An explicit XID never
// falls back to portal consent, and an unavailable target never yields a
// session that cannot be polled.
[[nodiscard]] CaptureResolution
resolve_capture(const CaptureRoute &route, std::string_view profile_id,
                std::uint64_t window, layerc::core::CaptureQuality quality,
                std::uint8_t sample_rate);

} // namespace layerc::daemon
