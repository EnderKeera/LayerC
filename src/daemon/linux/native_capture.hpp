#pragma once

// Platform-neutral capture seam for the Linux session engine. Native handles,
// consent, dispatch and transport lifetimes remain in the provider.
#include "layerc/capture/capture.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace layerc::daemon {

// One capture session per visual target, whatever its transport: the X11
// Composite adapter or a provider's portal stream. Sampling and evaluation
// stay in the visual service.
class VisualCaptureSession {
public:
  virtual ~VisualCaptureSession() = default;
  [[nodiscard]] virtual bool start() = 0;
  virtual void stop() noexcept = 0;
  virtual void set_quality(layerc::core::CaptureQuality quality) noexcept = 0;
  virtual void set_sample_rate(std::uint8_t rate) noexcept = 0;
  [[nodiscard]] virtual layerc::capture::CapturePoll poll(layerc::core::TimePoint now) = 0;
  [[nodiscard]] virtual layerc::runtime::RuntimeResourceStats resources() const noexcept = 0;
};

class NativeCaptureFactory {
public:
  virtual ~NativeCaptureFactory() = default;
  // A compositor report can explicitly identify an XWayland window; that
  // window still uses X11 capture rather than asking for portal consent.
  [[nodiscard]] virtual std::optional<std::uint64_t> x11_window(std::uint64_t window) const = 0;
  [[nodiscard]] virtual std::unique_ptr<VisualCaptureSession>
  create_capture(std::string_view profile_id, layerc::core::WindowId window,
                 layerc::core::CaptureQuality quality, std::uint8_t sample_rate) = 0;
};

} // namespace layerc::daemon
