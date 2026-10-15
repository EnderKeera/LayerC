#include "pipewire_frame.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

std::optional<WindowCanvas>
window_canvas(const LogicalRect &window, const LogicalRect &monitor,
              const std::uint32_t buffer_width, const std::uint32_t buffer_height) noexcept {
  if (window.width <= 0 || window.height <= 0 || monitor.width <= 0 || monitor.height <= 0 ||
      buffer_width == 0 || buffer_height == 0)
    return std::nullopt;
  const auto to_buffer = [](const std::int64_t logical, const std::int64_t logical_size,
                            const std::uint32_t buffer_size) {
    const auto pixels = (logical * buffer_size + logical_size / 2) / logical_size;
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(pixels, 0, std::numeric_limits<std::uint32_t>::max()));
  };
  WindowCanvas canvas{
      to_buffer(window.width, monitor.width, buffer_width),
      to_buffer(window.height, monitor.height, buffer_height),
      to_buffer(std::max<std::int64_t>(0, static_cast<std::int64_t>(monitor.x) - window.x),
                monitor.width, buffer_width),
      to_buffer(std::max<std::int64_t>(0, static_cast<std::int64_t>(monitor.y) - window.y),
                monitor.height, buffer_height)};
  if (canvas.width == 0 || canvas.height == 0) return std::nullopt;
  return canvas;
}

std::shared_ptr<const capture::CaptureFrame>
convert_video_frame(const RawVideoFrame &raw, const core::CaptureQuality quality,
                    std::optional<capture::PixelBounds> crop,
                    const core::TimePoint captured_at, std::string &error,
                    const std::optional<WindowCanvas> canvas) {
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
  const auto placed = canvas.value_or(WindowCanvas{source_width, source_height, 0, 0});
  if (placed.width == 0 || placed.height == 0 || placed.x >= placed.width ||
      placed.y >= placed.height) {
    error = "pipewire: crop is outside the window canvas";
    return nullptr;
  }
  const auto [width, height] = capture::capture_sample_dimensions(placed.width, placed.height,
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
  // The crop's share of the canvas; pixels outside it stay black and are
  // excluded from evaluation by this region.
  const auto visible_width = std::min(source_width, placed.width - placed.x);
  const auto visible_height = std::min(source_height, placed.height - placed.y);
  frame->available_region = {static_cast<double>(placed.x) / placed.width,
                             static_cast<double>(placed.y) / placed.height,
                             static_cast<double>(visible_width) / placed.width,
                             static_cast<double>(visible_height) / placed.height};
  frame->pixels.resize(static_cast<std::size_t>(width) * height);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(raw.data.data());
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto canvas_y = sampled_coordinate(y, placed.height, height);
    if (canvas_y < placed.y || canvas_y - placed.y >= visible_height) continue;
    const auto row = static_cast<std::size_t>(bounds.top + canvas_y - placed.y) * raw.stride;
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto canvas_x = sampled_coordinate(x, placed.width, width);
      if (canvas_x < placed.x || canvas_x - placed.x >= visible_width) continue;
      const auto *pixel =
          bytes + row + static_cast<std::size_t>(bounds.left + canvas_x - placed.x) * 4U;
      // Alpha is ignored: overlays composite opaque content.
      frame->pixels[static_cast<std::size_t>(y) * width + x] =
          rgb_order ? core::RgbPixel{pixel[0], pixel[1], pixel[2]}
                    : core::RgbPixel{pixel[2], pixel[1], pixel[0]};
    }
  }
  return frame;
}

} // namespace layerc::platform
