#include "layerc/capture/window_capture.hpp"

#include "capture_backend.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::capture {

bool capture_window_bounds(const HWND window, RECT &bounds) noexcept {
  bounds = {};
  if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &bounds, sizeof(bounds))) &&
      bounds.right > bounds.left && bounds.bottom > bounds.top)
    return true;
  return GetWindowRect(window, &bounds) != FALSE &&
         bounds.right > bounds.left && bounds.bottom > bounds.top;
}

bool frame_is_nearly_black(const CaptureFrame &frame) noexcept {
  if (frame.pixels.empty())
    return true;
  std::size_t bright{};
  std::size_t sampled{};
  const auto sample = [&](const std::size_t index) {
    if (index >= frame.pixels.size())
      return;
    const auto pixel = frame.pixels[index];
    if (pixel.red > 3 || pixel.green > 3 || pixel.blue > 3)
      ++bright;
    ++sampled;
  };

  const auto step = std::max<std::size_t>(1, frame.pixels.size() / 4'096U);
  for (std::size_t index = 0; index < frame.pixels.size(); index += step) {
    sample(index);
  }

  const auto expected_pixels =
      static_cast<std::uint64_t>(frame.width) * frame.height;
  if (frame.width != 0 && frame.height != 0 &&
      expected_pixels <= frame.pixels.size()) {
    const std::size_t width = frame.width;
    const std::size_t height = frame.height;
    const std::size_t perimeter = height == 1 ? width
                                  : width == 1
                                      ? height
                                      : 2U * width + 2U * (height - 2U);
    const auto border_step = std::max<std::size_t>(1, perimeter / 1'024U);
    for (std::size_t offset = 0; offset < perimeter; offset += border_step) {
      std::size_t x{};
      std::size_t y{};
      if (height == 1) {
        x = offset;
      } else if (width == 1) {
        y = offset;
      } else if (offset < width) {
        x = offset;
      } else if (offset < width + height - 2U) {
        x = width - 1U;
        y = offset - width + 1U;
      } else if (offset < 2U * width + height - 2U) {
        x = 2U * width + height - 3U - offset;
        y = height - 1U;
      } else {
        y = perimeter - offset;
      }
      sample(y * width + x);
    }
  }
  return bright * 1'000U < sampled * 5U;
}

class WindowCaptureSession::Impl {
public:
  Impl(const HWND window, const CaptureOptions options)
      : window_(window), options_(options) {}

  bool start() {
    stop();
    backend_ = make_wgc_backend(window_);
    if (backend_->start()) {
      backend_->set_quality(options_.quality);
      apply_exclusions();
      return true;
    }
    const auto wgc_error = backend_->last_error();
    backend_.reset();
    if (!options_.allow_desktop_duplication) {
      error_ = wgc_error;
      return false;
    }
    backend_ = make_desktop_duplication_backend(window_);
    if (!backend_->start()) {
      error_ = "WGC: " + wgc_error +
               "; Desktop Duplication: " + backend_->last_error();
      backend_.reset();
      return false;
    }
    backend_->set_quality(options_.quality);
    fallback_reason_ = wgc_error;
    apply_exclusions();
    return true;
  }

  void stop() {
    if (backend_)
      backend_->stop();
    backend_.reset();
    error_.clear();
    fallback_reason_.clear();
  }

  CapturePollResult poll(const core::TimePoint now, const bool foreground) {
    if (!backend_)
      return {.availability = core::CaptureAvailability::unavailable,
              .backend = core::CaptureBackendKind::none,
              .diagnostic = error_.empty() ? "capture is not started" : error_,
              .frame = {}};
    auto result = backend_->poll(now, foreground);
    if (result.availability == core::CaptureAvailability::unavailable &&
        result.backend == core::CaptureBackendKind::windows_graphics_capture &&
        options_.allow_desktop_duplication && IsWindow(window_)) {
      fallback_reason_ = result.diagnostic;
      backend_->stop();
      backend_ = make_desktop_duplication_backend(window_);
      if (backend_->start()) {
        backend_->set_quality(options_.quality);
        apply_exclusions();
        return {.availability = foreground
                                    ? core::CaptureAvailability::starting
                                    : core::CaptureAvailability::inactive,
                .backend = core::CaptureBackendKind::desktop_duplication,
                .diagnostic = "WGC became unavailable (" + fallback_reason_ +
                              "); Desktop Duplication fallback is starting",
                .frame = {}};
      }
      error_ = "WGC: " + fallback_reason_ +
               "; Desktop Duplication: " + backend_->last_error();
      backend_.reset();
      return {.availability = core::CaptureAvailability::unavailable,
              .backend = core::CaptureBackendKind::none,
              .diagnostic = error_,
              .frame = {}};
    }
    if (result.backend == core::CaptureBackendKind::desktop_duplication &&
        !fallback_reason_.empty())
      result.diagnostic += "; WGC fallback reason: " + fallback_reason_;
    return result;
  }

  void set_exclusions(const std::span<const RECT> rectangles) {
    excluded_.assign(rectangles.begin(), rectangles.end());
    apply_exclusions();
  }

  void set_quality(const core::CaptureQuality quality) {
    options_.quality = quality;
    if (backend_)
      backend_->set_quality(quality);
  }

  core::CaptureBackendKind kind() const noexcept {
    return backend_ ? backend_->kind() : core::CaptureBackendKind::none;
  }
  std::string gpu_description() const {
    return backend_ ? backend_->gpu_description() : std::string{};
  }

private:
  void apply_exclusions() {
    if (backend_)
      backend_->set_excluded_screen_rects(excluded_);
  }

  HWND window_{};
  CaptureOptions options_;
  std::unique_ptr<CaptureBackend> backend_;
  std::vector<RECT> excluded_;
  std::string error_;
  std::string fallback_reason_;
};

WindowCaptureSession::WindowCaptureSession(const HWND window,
                                           const CaptureOptions options)
    : impl_(std::make_unique<Impl>(window, options)) {}
WindowCaptureSession::~WindowCaptureSession() = default;
WindowCaptureSession::WindowCaptureSession(WindowCaptureSession &&) noexcept =
    default;
WindowCaptureSession &
WindowCaptureSession::operator=(WindowCaptureSession &&) noexcept = default;

bool WindowCaptureSession::start() { return impl_->start(); }
void WindowCaptureSession::stop() { impl_->stop(); }
CapturePollResult WindowCaptureSession::poll(const core::TimePoint now,
                                             const bool foreground) {
  return impl_->poll(now, foreground);
}
void WindowCaptureSession::set_excluded_screen_rects(
    const std::span<const RECT> rectangles) {
  impl_->set_exclusions(rectangles);
}
void WindowCaptureSession::set_quality(const core::CaptureQuality quality) {
  impl_->set_quality(quality);
}
core::CaptureBackendKind WindowCaptureSession::backend() const noexcept {
  return impl_->kind();
}
std::string WindowCaptureSession::gpu_description() const {
  return impl_->gpu_description();
}

bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const core::NormalizedRegion &available_region) noexcept {
  constexpr double epsilon = 1e-9;
  return rule_region.x >= available_region.x - epsilon &&
         rule_region.y >= available_region.y - epsilon &&
         rule_region.x + rule_region.width <=
             available_region.x + available_region.width + epsilon &&
         rule_region.y + rule_region.height <=
             available_region.y + available_region.height + epsilon;
}

bool region_is_available(const core::NormalizedRegion &rule_region,
                         const CaptureFrame &frame) noexcept {
  if (!region_is_available(rule_region, frame.available_region))
    return false;
  constexpr double epsilon = 1e-9;
  return std::ranges::none_of(
      frame.unavailable_regions, [&](const core::NormalizedRegion &excluded) {
        return rule_region.x < excluded.x + excluded.width - epsilon &&
               rule_region.x + rule_region.width > excluded.x + epsilon &&
               rule_region.y < excluded.y + excluded.height - epsilon &&
               rule_region.y + rule_region.height > excluded.y + epsilon;
      });
}

PixelBounds transform_output_crop(const PixelBounds logical_crop,
                                  const std::uint32_t logical_output_width,
                                  const std::uint32_t logical_output_height,
                                  const OutputRotation rotation) noexcept {
  switch (rotation) {
  case OutputRotation::rotate90:
    return {logical_crop.top, logical_output_width - logical_crop.right,
            logical_crop.bottom, logical_output_width - logical_crop.left};
  case OutputRotation::rotate180:
    return {logical_output_width - logical_crop.right,
            logical_output_height - logical_crop.bottom,
            logical_output_width - logical_crop.left,
            logical_output_height - logical_crop.top};
  case OutputRotation::rotate270:
    return {logical_output_height - logical_crop.bottom, logical_crop.left,
            logical_output_height - logical_crop.top, logical_crop.right};
  case OutputRotation::identity:
  default:
    return logical_crop;
  }
}

std::pair<std::uint32_t, std::uint32_t>
capture_sample_dimensions(const std::uint32_t width, const std::uint32_t height,
                          const core::CaptureQuality quality) noexcept {
  if (width == 0 || height == 0)
    return {0, 0};
  std::uint32_t limit{};
  switch (quality) {
  case core::CaptureQuality::low:
    limit = 64U;
    break;
  case core::CaptureQuality::balanced:
    limit = 256U;
    break;
  case core::CaptureQuality::exact:
    limit = 0U;
    break;
  }
  if (limit == 0 || (width <= limit && height <= limit))
    return {width, height};
  const double scale = std::min(static_cast<double>(limit) / width,
                                static_cast<double>(limit) / height);
  return {std::max<std::uint32_t>(
              1, static_cast<std::uint32_t>(std::lround(width * scale))),
          std::max<std::uint32_t>(
              1, static_cast<std::uint32_t>(std::lround(height * scale)))};
}

} // namespace layerc::capture
