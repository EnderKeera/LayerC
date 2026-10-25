#pragma once

#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace layerc::capture {

enum class OutputRotation : std::uint8_t {
  identity,
  rotate90,
  rotate180,
  rotate270,
};

struct PixelBounds {
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t right{};
  std::uint32_t bottom{};

  bool operator==(const PixelBounds &) const = default;
};

struct CaptureFrame {
  std::vector<core::RgbPixel> pixels;
  std::uint32_t width{};
  std::uint32_t height{};
  // Desktop Duplication can only supply the portion of a window on its chosen
  // output. Full-window capture reports the normalized region below.
  core::NormalizedRegion available_region{0.0, 0.0, 1.0, 1.0};
  // Regions hidden because LayerC overlays occupy those screen pixels. Rules
  // intersecting one are unavailable rather than evaluated against synthetic
  // black pixels.
  std::vector<core::NormalizedRegion> unavailable_regions;
  core::TimePoint captured_at{};

  [[nodiscard]] core::PixelFrameView view() const noexcept {
    return {pixels, width, height, width};
  }
};

enum class CaptureRecovery : std::uint8_t {
  none,
  restart_backend,
};

struct CapturePollResult {
  core::CaptureAvailability availability{core::CaptureAvailability::inactive};
  core::CaptureBackendKind backend{core::CaptureBackendKind::none};
  std::string diagnostic{};
  std::shared_ptr<const CaptureFrame> frame{};
  CaptureRecovery recovery{CaptureRecovery::none};

  [[nodiscard]] core::CaptureHealth health() const {
    return {availability, backend, diagnostic};
  }
};

// Owns capture availability policy that must be identical for every backend.
// A frame is counted once by identity, so polling the same buffered frame does
// not advance the protected-content threshold.
class CaptureHealthTracker {
public:
  static constexpr std::uint32_t protected_frame_threshold = 30;

  [[nodiscard]] CapturePollResult observe(CapturePollResult result);
  void reset() noexcept;
  [[nodiscard]] std::uint32_t consecutive_black_frames() const noexcept {
    return black_frames_;
  }

private:
  std::shared_ptr<const CaptureFrame> observed_frame_;
  std::uint32_t black_frames_{};
};

struct CapturePoll {
  core::WindowId window;
  CapturePollResult result;
};

// Shared boundary results expose a frame only while it is available and valid.
[[nodiscard]] CapturePoll normalize_capture_poll(core::WindowId window,
                                                 CapturePollResult result);

[[nodiscard]] bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const core::NormalizedRegion &available_region) noexcept;
[[nodiscard]] bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const CaptureFrame &frame) noexcept;
[[nodiscard]] PixelBounds transform_output_crop(
    PixelBounds logical_crop, std::uint32_t logical_output_width,
    std::uint32_t logical_output_height, OutputRotation rotation) noexcept;
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
capture_sample_dimensions(std::uint32_t width, std::uint32_t height,
                          core::CaptureQuality quality) noexcept;
// Source coordinate under the center of output cell `index` when an axis of
// `source_extent` pixels is resampled to `output_extent` cells.
[[nodiscard]] std::uint32_t sampled_coordinate(std::uint32_t index,
                                               std::uint32_t source_extent,
                                               std::uint32_t output_extent) noexcept;
[[nodiscard]] bool frame_is_nearly_black(const CaptureFrame &frame) noexcept;

} // namespace layerc::capture
