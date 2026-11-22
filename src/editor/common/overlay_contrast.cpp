#include "overlay_contrast.hpp"

#include "editor_color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace layerc::editor {
namespace {

std::string ratio_text(const double ratio) {
  std::array<char, 24> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.1f:1",
                std::floor(std::max(ratio, 1.0) * 10.0) / 10.0);
  return buffer.data();
}

std::string minimum_text(const double minimum) {
  std::array<char, 16> buffer{};
  if (minimum == std::floor(minimum))
    std::snprintf(buffer.data(), buffer.size(), "%d", static_cast<int>(minimum));
  else
    std::snprintf(buffer.data(), buffer.size(), "%.1f", minimum);
  return buffer.data();
}

} // namespace

std::string overlay_contrast_line(const std::uint32_t fill, const std::uint32_t background,
                                  const std::uint32_t border, const std::uint32_t text,
                                  const bool show_text) {
  std::string line;
  if (show_text) {
    std::string pairs;
    for (const auto &[name, color] :
         {std::pair{"Fill", fill}, std::pair{"Background", background}}) {
      const double ratio = rgb_contrast_ratio(text, color);
      if (ratio >= overlay_text_minimum) continue;
      pairs += pairs.empty() ? "Text is " : " and ";
      pairs += ratio_text(ratio) + " on " + name;
    }
    if (!pairs.empty())
      line += " " + pairs + "; aim for " + minimum_text(overlay_text_minimum) + ":1.";
  }
  if (const double ratio = rgb_contrast_ratio(border, background);
      ratio < overlay_mark_minimum)
    line += " Border is " + ratio_text(ratio) + " on Background; aim for " +
            minimum_text(overlay_mark_minimum) + ":1.";
  return line.empty() ? line : "Low contrast." + line;
}

} // namespace layerc::editor
