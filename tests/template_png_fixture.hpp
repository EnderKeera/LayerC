#pragma once

#include "layerc/core/hash.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/platform/template_png.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {

// A structured, fully opaque template image that passes asset validation.
inline layerc::core::TemplateImage checker_template_image(
    const std::uint32_t width = 16, const std::uint32_t height = 16) {
  layerc::core::TemplateImage image;
  image.width = width;
  image.height = height;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const bool light = ((x / 4U) + (y / 4U)) % 2U == 0U;
      image.pixels.push_back(
          {static_cast<std::uint8_t>(light ? 230U : 20U),
           static_cast<std::uint8_t>((x * 13U) & 0xffU),
           static_cast<std::uint8_t>((y * 17U) & 0xffU)});
      image.alpha.push_back(0xffU);
    }
  }
  return image;
}

struct TemplatePngFixture {
  std::string bytes;
  std::string sha256;
};

// Canonical PNG bytes of checker_template_image() and their content hash.
inline const TemplatePngFixture &canonical_template_png_fixture() {
  static const TemplatePngFixture fixture = [] {
    const auto encoded =
        layerc::platform::encode_template_png(checker_template_image());
    const auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
    return TemplatePngFixture{
        .bytes = std::string(bytes.begin(), bytes.end()),
        .sha256 = layerc::core::sha256(std::as_bytes(std::span(bytes)))};
  }();
  return fixture;
}

} // namespace layerc::test
