#include "editor_color.hpp"

#include <algorithm>
#include <cmath>

namespace layerc::editor {
namespace {

unsigned channel(const std::uint32_t color, const unsigned shift) {
  return (color >> shift) & 0xFFU;
}

} // namespace

double rgb_relative_luminance(const std::uint32_t color) noexcept {
  const auto component = [](const std::uint32_t value) {
    const double part = static_cast<double>(value) / 255.0;
    return part <= 0.04045 ? part / 12.92 : std::pow((part + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * component(channel(color, 16U)) +
         0.7152 * component(channel(color, 8U)) + 0.0722 * component(channel(color, 0U));
}

double rgb_contrast_ratio(const std::uint32_t first, const std::uint32_t second) noexcept {
  const double one = rgb_relative_luminance(first);
  const double two = rgb_relative_luminance(second);
  return (std::max(one, two) + 0.05) / (std::min(one, two) + 0.05);
}

double rgb_hue_degrees(const std::uint32_t color) noexcept {
  const int red = static_cast<int>(channel(color, 16U));
  const int green = static_cast<int>(channel(color, 8U));
  const int blue = static_cast<int>(channel(color, 0U));
  const int high = std::max({red, green, blue});
  const int chroma = high - std::min({red, green, blue});
  if (chroma == 0) return 0.0;
  double sector{};
  if (high == red) sector = static_cast<double>(green - blue) / chroma;
  else if (high == green) sector = static_cast<double>(blue - red) / chroma + 2.0;
  else sector = static_cast<double>(red - green) / chroma + 4.0;
  const double hue = sector * 60.0;
  return hue < 0.0 ? hue + 360.0 : hue;
}

std::uint32_t rgb_accent_2(const std::uint32_t accent) noexcept {
  const double distance = rgb_hue_degrees(accent) - rgb_hue_degrees(accent_2_blue);
  const double gap = std::abs(distance);
  return std::min(gap, 360.0 - gap) <= 45.0 ? accent_2_violet : accent_2_blue;
}

std::uint32_t rgb_on_accent(const std::uint32_t accent) noexcept {
  const std::uint32_t luma = channel(accent, 16U) * 299U + channel(accent, 8U) * 587U +
                             channel(accent, 0U) * 114U;
  return luma >= 140000U ? 0x141414U : 0xFFFFFFU;
}

} // namespace layerc::editor
