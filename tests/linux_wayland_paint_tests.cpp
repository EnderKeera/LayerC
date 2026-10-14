// The layer-shell painter without a compositor: display-list ops land on the
// exact device pixels at integer buffer scales.

#include "wayland_overlay.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using layerc::test::expect;
using namespace layerc::platform;

struct Canvas {
  int width{};
  int height{};
  std::vector<unsigned char> bytes;

  Canvas(int w, int h)
      : width(w), height(h), bytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0xAB) {}
  [[nodiscard]] std::uint32_t at(int x, int y) const {
    std::uint32_t value{};
    std::memcpy(&value,
                bytes.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)) * 4,
                sizeof(value));
    return value & 0xFFFFFF;
  }
};

OverlayDisplayList panel() {
  OverlayDisplayList list;
  list.bounds = {500, 300, 20, 10};
  list.ops.push_back({OverlayOpKind::fill_rect, {0, 0, 20, 10}, 0x102030, {}, 0});
  list.ops.push_back({OverlayOpKind::fill_rect, {2, 2, 6, 4}, 0xFF0000, {}, 0});
  list.ops.push_back({OverlayOpKind::stroke_rect, {10, 0, 10, 10}, 0x00FF00, {}, 0});
  return list;
}

void paints_fills_and_one_pixel_outlines_at_scale_one() {
  Canvas canvas(20, 10);
  paint_overlay_display_list(panel(), canvas.bytes.data(), 20, 10, 80, 1);
  expect(canvas.at(0, 0) == 0x102030 && canvas.at(9, 9) == 0x102030,
         "background fill covers the panel");
  expect(canvas.at(2, 2) == 0xFF0000 && canvas.at(7, 5) == 0xFF0000 &&
             canvas.at(8, 5) == 0x102030 && canvas.at(1, 1) == 0x102030,
         "fills cover exactly their rect");
  expect(canvas.at(10, 0) == 0x00FF00 && canvas.at(19, 9) == 0x00FF00 &&
             canvas.at(10, 5) == 0x00FF00 && canvas.at(15, 5) == 0x102030,
         "outlines are one pixel wide and inside their rect");
}

void scales_logical_ops_to_device_pixels() {
  Canvas canvas(40, 20);
  paint_overlay_display_list(panel(), canvas.bytes.data(), 40, 20, 160, 2);
  expect(canvas.at(4, 4) == 0xFF0000 && canvas.at(15, 11) == 0xFF0000 &&
             canvas.at(16, 11) == 0x102030 && canvas.at(3, 3) == 0x102030,
         "a logical fill doubles in device pixels");
  expect(canvas.at(20, 0) == 0x00FF00 && canvas.at(39, 19) == 0x00FF00 &&
             canvas.at(21, 1) == 0x102030 && canvas.at(30, 10) == 0x102030,
         "outlines stay one device pixel wide at scale 2");
}

void draws_centered_text_inside_its_rect() {
  OverlayDisplayList list;
  list.bounds = {0, 0, 60, 20};
  list.ops.push_back({OverlayOpKind::fill_rect, {0, 0, 60, 20}, 0x000000, {}, 0});
  list.ops.push_back({OverlayOpKind::text, {0, 0, 60, 20}, 0xFFFFFF, "Ready", 13});
  Canvas canvas(60, 20);
  paint_overlay_display_list(list, canvas.bytes.data(), 60, 20, 240, 1);
  int lit = 0;
  int left = 60;
  int right = -1;
  for (int y = 0; y < 20; ++y)
    for (int x = 0; x < 60; ++x)
      if (canvas.at(x, y) != 0) {
        ++lit;
        left = std::min(left, x);
        right = std::max(right, x);
      }
  expect(lit > 10, "text paints glyph pixels");
  expect(left > 0 && right < 59 && (left + right) / 2 >= 22 && (left + right) / 2 <= 37,
         "text is centred horizontally inside its rect");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(paints_fills_and_one_pixel_outlines_at_scale_one),
                 LAYERC_TEST_CASE(scales_logical_ops_to_device_pixels),
                 LAYERC_TEST_CASE(draws_centered_text_inside_its_rect))
