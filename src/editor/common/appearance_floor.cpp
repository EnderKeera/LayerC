#include "appearance_floor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace layerc::editor {
namespace {

std::string floor_label(const double minimum) {
  std::array<char, 16> buffer{};
  if (minimum == std::floor(minimum))
    std::snprintf(buffer.data(), buffer.size(), "%d", static_cast<int>(minimum));
  else
    std::snprintf(buffer.data(), buffer.size(), "%.1f", minimum);
  return buffer.data();
}

} // namespace

double contrast_floor(const ContrastPairKind pair) noexcept {
  return pair == ContrastPairKind::disabled_fill ? 3.0 : 4.5;
}

std::string_view contrast_pair_label(const ContrastPairKind pair) noexcept {
  switch (pair) {
  case ContrastPairKind::disabled_fill: return "Disabled / Fill";
  case ContrastPairKind::text_hover: return "Text / Hover";
  case ContrastPairKind::text_selection: return "Text / Selection";
  case ContrastPairKind::text_fill: break;
  }
  return "Text / Fill";
}

std::string_view primary_button_surface(const ContrastPairKind pair) noexcept {
  switch (pair) {
  case ContrastPairKind::text_hover: return "primary button hover fill";
  case ContrastPairKind::text_selection: return "primary button pressed fill";
  default: return "primary button accent";
  }
}

std::string contrast_ratio_label(const double ratio) {
  std::array<char, 24> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.1f:1",
                std::floor(std::max(ratio, 1.0) * 10.0) / 10.0);
  return buffer.data();
}

std::string contrast_refusal_text(const std::string_view role_label,
                                  const ContrastPairKind pair, const double ratio,
                                  const std::string_view surface) {
  std::string text(role_label);
  text += ": ";
  text += contrast_pair_label(pair);
  text += " contrast is ";
  text += contrast_ratio_label(ratio);
  if (!surface.empty()) {
    text += " on the ";
    text += surface;
  }
  text += "; at least ";
  text += floor_label(contrast_floor(pair));
  text += ":1 is required.";
  return text;
}

} // namespace layerc::editor
