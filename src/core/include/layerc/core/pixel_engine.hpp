#pragma once

#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace layerc::core {

struct RgbPixel {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};

  bool operator==(const RgbPixel &) const = default;
};

// Shared Rec. 709 luma coefficients used by the template matcher's sample
// preparation and the pixel evaluator's brightness predicate.
[[nodiscard]] inline double rgb_luminance(double red, double green,
                                          double blue) noexcept {
  return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}
[[nodiscard]] inline double rgb_luminance(const RgbPixel pixel) noexcept {
  return rgb_luminance(pixel.red, pixel.green, pixel.blue);
}

struct PixelFrameView {
  std::span<const RgbPixel> pixels;
  std::uint32_t width{};
  std::uint32_t height{};
  // Number of RgbPixel elements between the start of adjacent rows. Zero
  // means a tightly packed frame whose stride equals width.
  std::uint32_t row_stride{};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint32_t stride() const noexcept {
    return row_stride == 0 ? width : row_stride;
  }
};

struct QualityProfile {
  std::uint32_t pixel_axis;
  std::uint32_t template_axis; // Zero means full reference resolution.
  std::size_t search_samples;
  std::size_t comparisons;
};

inline constexpr std::array<QualityProfile, 3> quality_profiles{{
    {64U, 32U, 128U, 512U * 1024U},
    {256U, 64U, 256U, 1024U * 1024U},
    {std::numeric_limits<std::uint32_t>::max(), 0U, 512U, 8U * 1024U * 1024U},
}};

[[nodiscard]] constexpr const QualityProfile &quality_profile(
    const CaptureQuality quality) noexcept {
  return quality_profiles[static_cast<std::size_t>(quality)];
}

struct PixelEvaluation {
  bool matched{};
  std::uint32_t average_color{};
  double average_brightness{};
  double color_coverage{};
  std::size_t sampled_pixels{};
};

[[nodiscard]] PixelEvaluation evaluate_pixel_rule(
    const PixelRule &rule, PixelFrameView frame, CaptureQuality quality);

struct ObservationMeasurement {
  std::string observation_id;
  CaptureAvailability availability{CaptureAvailability::inactive};
  bool matched{};
  double confidence{};
  TimePoint sampled_at{};
};

} // namespace layerc::core
