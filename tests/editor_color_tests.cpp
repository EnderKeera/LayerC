#include "editor_color.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>

// The shared color math both editors paint with (DesignSystem.md, Tokens and
// roles): WCAG contrast, hue, the second accent mark and ink on an accent.
namespace {

using layerc::editor::accent_2_blue;
using layerc::editor::accent_2_violet;
using layerc::editor::rgb_accent_2;
using layerc::editor::rgb_contrast_ratio;
using layerc::editor::rgb_hue_degrees;
using layerc::editor::rgb_on_accent;
using layerc::test::expect;

bool near(const double first, const double second, const double tolerance = 1e-9) {
  return first > second - tolerance && first < second + tolerance;
}

} // namespace

void editor_color_contracts() {
  expect(near(rgb_contrast_ratio(0x000000, 0xFFFFFF), 21.0),
         "black on white is the maximum 21:1");
  expect(near(rgb_contrast_ratio(0xDDBC17, 0x1F1F1F),
              rgb_contrast_ratio(0x1F1F1F, 0xDDBC17)),
         "the ratio is symmetric");
  expect(near(rgb_contrast_ratio(0x4B7BEC, 0x4B7BEC), 1.0),
         "a color against itself is 1:1");

  expect(near(rgb_hue_degrees(0x000000), 0.0) && near(rgb_hue_degrees(0x808080), 0.0) &&
             near(rgb_hue_degrees(0xFFFFFF), 0.0),
         "grays have no hue");
  const double red = rgb_hue_degrees(0xFF0000);
  const double magenta = rgb_hue_degrees(0xFF00FF);
  const double near_wrap = rgb_hue_degrees(0xFF0080);
  expect(near(red, 0.0) && near(magenta, 300.0) && near_wrap > 300.0 && near_wrap < 360.0,
         "hue spans [0, 360) and wraps below red");

  // The blue accent_2 sits at about 209.8 degrees; the 45 degree window is the
  // rule in both directions.
  const double blue_hue = rgb_hue_degrees(accent_2_blue);
  expect(blue_hue > 200.0 && blue_hue < 220.0, "the blue accent_2 has its blue hue");
  const auto hue_at = [](const double degrees) {
    // A fully saturated color at `degrees` on the hue circle.
    const double sector = degrees / 60.0;
    const auto channel = [](const double value) {
      return static_cast<std::uint32_t>(value * 255.0 + (value >= 0.0 ? 0.5 : -0.5));
    };
    const double x = 1.0 - std::abs(std::fmod(sector, 2.0) - 1.0);
    double r{}, g{}, b{};
    if (sector < 1.0) { r = 1; g = x; }
    else if (sector < 2.0) { r = x; g = 1; }
    else if (sector < 3.0) { g = 1; b = x; }
    else if (sector < 4.0) { g = x; b = 1; }
    else if (sector < 5.0) { r = x; b = 1; }
    else { r = 1; b = x; }
    return (channel(r) << 16U) | (channel(g) << 8U) | channel(b);
  };
  for (const double inside : {44.0, -44.0}) {
    const auto color = hue_at(blue_hue + inside);
    expect(near(rgb_hue_degrees(color), blue_hue + inside, 0.6) &&
               rgb_accent_2(color) == accent_2_violet,
           "one degree inside the window takes the violet mark");
  }
  for (const double outside : {46.0, -46.0}) {
    const auto color = hue_at(blue_hue + outside);
    expect(near(rgb_hue_degrees(color), blue_hue + outside, 0.6) &&
               rgb_accent_2(color) == accent_2_blue,
           "one degree outside the window takes the blue mark");
  }

  constexpr std::uint32_t copper = 0xD88E4B;
  constexpr std::uint32_t teal = 0x5AB6AA;
  constexpr std::uint32_t plum = 0xB382E2;
  constexpr std::uint32_t slate = 0x8FA3B8;
  expect(rgb_accent_2(copper) == accent_2_blue && rgb_accent_2(plum) == accent_2_blue,
         "copper and plum keep the blue mark");
  expect(rgb_accent_2(teal) == accent_2_violet && rgb_accent_2(slate) == accent_2_violet &&
             rgb_accent_2(0x0078D4) == accent_2_violet,
         "teal, slate and the Windows blue take the violet mark");
  expect(rgb_accent_2(0xE81123) == accent_2_blue, "a red accent keeps the blue mark");

  expect(rgb_on_accent(0xE9E4DD) == 0x141414 && rgb_on_accent(0x14161A) == 0xFFFFFF,
         "ink on an accent follows the light/dark threshold");
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(editor_color_contracts))
