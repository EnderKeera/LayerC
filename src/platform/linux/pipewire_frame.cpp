#include "pipewire_frame.hpp"

#include <algorithm>
#include <cmath>

namespace layerc::platform {
namespace {

[[nodiscard]] std::uint32_t sampled_coordinate(const std::uint32_t index,
                                               const std::uint32_t source,
                                               const std::uint32_t output) noexcept {
  return std::min<std::uint32_t>(
      source - 1U, static_cast<std::uint32_t>(
                       (static_cast<std::uint64_t>(index) * source + source / 2U) / output));
}

[[nodiscard]] std::uint32_t scale(const std::int64_t logical, const std::int64_t logical_size,
                                  const std::uint32_t buffer_size) noexcept {
  const auto clamped = std::clamp<std::int64_t>(logical, 0, logical_size);
  return static_cast<std::uint32_t>(
      (clamped * static_cast<std::int64_t>(buffer_size) + logical_size / 2) / logical_size);
}

} // namespace

std::optional<capture::PixelBounds>
monitor_crop(const LogicalRect &window, const LogicalRect &monitor,
             const std::uint32_t buffer_width, const std::uint32_t buffer_height) noexcept {
  if (window.width <= 0 || window.height <= 0 || monitor.width <= 0 || monitor.height <= 0 ||
      buffer_width == 0 || buffer_height == 0)
    return std::nullopt;
  const std::int64_t left = static_cast<std::int64_t>(window.x) - monitor.x;
  const std::int64_t top = static_cast<std::int64_t>(window.y) - monitor.y;
  const std::int64_t right = left + window.width;
  const std::int64_t bottom = top + window.height;
  if (right <= 0 || bottom <= 0 || left >= monitor.width || top >= monitor.height)
    return std::nullopt;
  capture::PixelBounds bounds{scale(left, monitor.width, buffer_width),
                              scale(top, monitor.height, buffer_height),
                              scale(right, monitor.width, buffer_width),
                              scale(bottom, monitor.height, buffer_height)};
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return std::nullopt;
  return bounds;
}

std::shared_ptr<const capture::CaptureFrame>
convert_video_frame(const RawVideoFrame &raw, const core::CaptureQuality quality,
                    std::optional<capture::PixelBounds> crop,
                    const core::TimePoint captured_at, std::string &error) {
  if (raw.width == 0 || raw.height == 0) {
    error = "pipewire: empty video buffer";
    return nullptr;
  }
  if (raw.stride < static_cast<std::uint64_t>(raw.width) * 4U ||
      raw.data.size() < static_cast<std::uint64_t>(raw.stride) * (raw.height - 1U) +
                            static_cast<std::uint64_t>(raw.width) * 4U) {
    error = "pipewire: video buffer is shorter than its geometry";
    return nullptr;
  }
  const capture::PixelBounds whole{0, 0, raw.width, raw.height};
  auto bounds = crop.value_or(whole);
  bounds.right = std::min(bounds.right, raw.width);
  bounds.bottom = std::min(bounds.bottom, raw.height);
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    error = "pipewire: crop is outside the video buffer";
    return nullptr;
  }
  const auto source_width = bounds.right - bounds.left;
  const auto source_height = bounds.bottom - bounds.top;
  const auto [width, height] = capture::capture_sample_dimensions(source_width, source_height,
                                                                  quality);
  if (width == 0 || height == 0) {
    error = "pipewire: sampled dimensions are empty";
    return nullptr;
  }
  const bool rgb_order = raw.format == VideoPixelFormat::rgbx ||
                         raw.format == VideoPixelFormat::rgba;
  auto frame = std::make_shared<capture::CaptureFrame>();
  frame->width = width;
  frame->height = height;
  frame->captured_at = captured_at;
  frame->pixels.resize(static_cast<std::size_t>(width) * height);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw.data.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto row = static_cast<std::size_t>(bounds.top + sampled_coordinate(y, source_height,
                                                                              height)) *
                     raw.stride;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto *pixel =
          bytes + row +
          static_cast<std::size_t>(bounds.left + sampled_coordinate(x, source_width, width)) * 4U;
      // Alpha is ignored: overlays composite opaque content.
      frame->pixels[static_cast<std::size_t>(y) * width + x] =
          rgb_order ? core::RgbPixel{pixel[0], pixel[1], pixel[2]}
                    : core::RgbPixel{pixel[2], pixel[1], pixel[0]};
    }
  }
  return frame;
}

} // namespace layerc::platform
