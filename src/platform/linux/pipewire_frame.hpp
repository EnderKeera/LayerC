#pragma once

// Conversion of one negotiated PipeWire video buffer (MemPtr/MemFd, no
// DMA-BUF) into capture::CaptureFrame. Pure: no PipeWire header reaches this
// file, so formats, strides, crops, and sampling are testable without a
// PipeWire daemon.

#include "layerc/capture/capture.hpp"
#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace layerc::platform {

enum class VideoPixelFormat : std::uint8_t { bgrx, bgra, rgbx, rgba };

struct RawVideoFrame {
  std::span<const std::byte> data;
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride{};
  VideoPixelFormat format{VideoPixelFormat::bgrx};
};

struct LogicalRect {
  int x{};
  int y{};
  int width{};
  int height{};
};

// The buffer pixels of `window` inside a monitor stream whose logical
// geometry is `monitor`, clamped to the buffer. nullopt when the window does
// not intersect the monitor (capture reports outside_output).
[[nodiscard]] std::optional<capture::PixelBounds>
monitor_crop(const LogicalRect &window, const LogicalRect &monitor,
             std::uint32_t buffer_width, std::uint32_t buffer_height) noexcept;

// The whole window at buffer scale, with the origin of its monitor crop. A
// partially visible window keeps whole-window coordinates: the crop is placed
// at (x, y) and the rest of the canvas is reported unavailable.
struct WindowCanvas {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t x{};
  std::uint32_t y{};
};

[[nodiscard]] std::optional<WindowCanvas>
window_canvas(const LogicalRect &window, const LogicalRect &monitor,
              std::uint32_t buffer_width, std::uint32_t buffer_height) noexcept;

// Samples `crop` (default: the whole buffer) at `quality`. With `canvas`, the
// frame spans the whole window and `available_region` covers only the crop.
// Returns null and sets `error` when the buffer is shorter than its stride and
// height claim.
[[nodiscard]] std::shared_ptr<const capture::CaptureFrame>
convert_video_frame(const RawVideoFrame &raw, core::CaptureQuality quality,
                    std::optional<capture::PixelBounds> crop, core::TimePoint captured_at,
                    std::string &error, std::optional<WindowCanvas> canvas = std::nullopt);

} // namespace layerc::platform
