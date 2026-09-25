#include "layerc/core/pixel_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace layerc::core {
namespace {

std::uint8_t channel(const std::uint32_t color, const unsigned shift) {
  return static_cast<std::uint8_t>((color >> shift) & 0xFFU);
}

bool color_matches(const RgbPixel pixel, const std::uint32_t target,
                   const std::uint8_t tolerance) {
  const auto within = [tolerance](const std::uint8_t value,
                                  const std::uint8_t expected) {
    return std::abs(static_cast<int>(value) - static_cast<int>(expected)) <=
           tolerance;
  };
  return within(pixel.red, channel(target, 16U)) &&
         within(pixel.green, channel(target, 8U)) &&
         within(pixel.blue, channel(target, 0U));
}

} // namespace

bool PixelFrameView::valid() const noexcept {
  const auto stride = row_stride == 0 ? width : row_stride;
  if (width == 0 || height == 0 || stride < width) return false;
  const auto required = static_cast<std::uint64_t>(stride) * height;
  return required <= pixels.size();
}

PixelEvaluation evaluate_pixel_rule(const PixelRule &rule,
                                    const PixelFrameView frame,
                                    const CaptureQuality quality) {
  PixelEvaluation result;
  if (!frame.valid()) return result;

  const auto stride = frame.stride();
  const auto [left, top, right, bottom] =
      to_pixel_bounds(rule.region, frame.width, frame.height);
  if (right <= left || bottom <= top) return result;

  const auto region_width = right - left;
  const auto region_height = bottom - top;
  const auto evaluation_axis_sample_bound = quality_profile(quality).pixel_axis;
  const auto samples_x =
      std::min(region_width, evaluation_axis_sample_bound);
  const auto samples_y =
      std::min(region_height, evaluation_axis_sample_bound);
  if (samples_x == 0 || samples_y == 0) return result;

  std::uint64_t red{};
  std::uint64_t green{};
  std::uint64_t blue{};
  std::uint64_t covered{};
  const auto *coverage = std::get_if<ColorCoveragePredicate>(&rule.predicate);
  for (std::uint32_t sample_y = 0; sample_y < samples_y; ++sample_y) {
    const auto y = top + static_cast<std::uint32_t>(
                             (static_cast<std::uint64_t>(sample_y) * 2U + 1U) *
                             region_height /
                             (static_cast<std::uint64_t>(samples_y) * 2U));
    for (std::uint32_t sample_x = 0; sample_x < samples_x; ++sample_x) {
      const auto x = left + static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(sample_x) * 2U + 1U) *
                                region_width /
                                (static_cast<std::uint64_t>(samples_x) * 2U));
      const auto pixel =
          frame.pixels[static_cast<std::size_t>(y) * stride + x];
      red += pixel.red;
      green += pixel.green;
      blue += pixel.blue;
      if (coverage != nullptr &&
          color_matches(pixel, coverage->target_color, coverage->tolerance))
        ++covered;
    }
  }

  result.sampled_pixels = static_cast<std::size_t>(samples_x) * samples_y;
  const auto count = static_cast<double>(result.sampled_pixels);
  const auto average_red = static_cast<double>(red) / count;
  const auto average_green = static_cast<double>(green) / count;
  const auto average_blue = static_cast<double>(blue) / count;
  const auto round_channel = [](const double value) {
    return static_cast<std::uint32_t>(
        std::clamp(std::lround(value), 0L, 255L));
  };
  result.average_color = (round_channel(average_red) << 16U) |
                         (round_channel(average_green) << 8U) |
                         round_channel(average_blue);
  result.average_brightness =
      rgb_luminance(average_red, average_green, average_blue);
  result.color_coverage = static_cast<double>(covered) / count;

  std::visit(
      [&](const auto &predicate) {
        using T = std::decay_t<decltype(predicate)>;
        if constexpr (std::is_same_v<T, AverageColorPredicate>) {
          const auto within = [tolerance = predicate.tolerance](
                                  const double actual,
                                  const std::uint8_t expected) {
            return std::abs(actual - static_cast<double>(expected)) <=
                   tolerance;
          };
          result.matched =
              within(average_red, channel(predicate.target_color, 16U)) &&
              within(average_green, channel(predicate.target_color, 8U)) &&
              within(average_blue, channel(predicate.target_color, 0U));
        } else if constexpr (std::is_same_v<T, BrightnessPredicate>) {
          result.matched = predicate.comparison == BrightnessComparison::above
                               ? result.average_brightness >= predicate.threshold
                               : result.average_brightness <= predicate.threshold;
        } else {
          result.matched =
              result.color_coverage >= predicate.minimum_fraction;
        }
      },
      rule.predicate);
  return result;
}

} // namespace layerc::core
