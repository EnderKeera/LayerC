// The layer-shell painter without a compositor: display-list ops land on the
// exact device pixels at integer buffer scales.

#include "display_types.hpp"
#include "wayland_overlay.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
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

constexpr std::uint32_t pip_color = 0x38A97C;
constexpr std::uint32_t accent_color = 0xD04A36;

// A pips counter (2 of 4) above an active indicator in the given flash phase,
// resolved the way the overlay service hands typed rows to every provider.
OverlayFrame typed_frame(const bool flash_on) {
  OverlayFrame frame;
  frame.target = {0, 0, 800, 600};
  frame.panel.id = "typed";
  frame.panel.layout = layerc::core::PanelLayout::column;
  frame.panel.style = layerc::core::PanelStyle::bar;
  layerc::core::ItemPresentation counter;
  counter.item_id = "stacks";
  counter.kind = layerc::core::OverlayItemKind::counter;
  counter.label = "Stacks  2/4";
  counter.has_progress = false;
  counter.pips = 4;
  counter.filled_pips = 2;
  counter.color = pip_color;
  layerc::core::ItemPresentation indicator;
  indicator.item_id = "power";
  indicator.kind = layerc::core::OverlayItemKind::indicator;
  indicator.label = "Power";
  indicator.has_progress = false;
  indicator.active = true;
  indicator.ready = false;
  indicator.flash_on = flash_on;
  indicator.color = accent_color;
  frame.bars.push_back(overlay_bar_from_presentation(counter));
  frame.bars.push_back(overlay_bar_from_presentation(indicator));
  return frame;
}

Canvas painted(const OverlayDisplayList &list) {
  Canvas canvas(list.bounds.width, list.bounds.height);
  paint_overlay_display_list(list, canvas.bytes.data(), canvas.width, canvas.height,
                             canvas.width * 4, 1);
  return canvas;
}

void paints_typed_pips_and_indicator_flash() {
  const auto on = build_overlay_display_list(typed_frame(true));
  const auto off = build_overlay_display_list(typed_frame(false));
  if (!expect(on.bounds.width > 0 && on.bounds.height > 0 && on.bounds == off.bounds,
              "both flash phases keep identical layer-surface bounds")) return;
  const auto canvas = painted(on);
  std::vector<OverlayRect> filled, empty;
  std::optional<OverlayRect> accent;
  for (const auto &op : on.ops) {
    if (op.rgb == pip_color && op.rect.width == op.rect.height)
      (op.kind == OverlayOpKind::fill_rect ? filled : empty).push_back(op.rect);
    if (op.rgb == accent_color && op.kind == OverlayOpKind::fill_rect) accent = op.rect;
  }
  if (!expect(filled.size() == 2 && empty.size() == 2, "two filled and two empty pips")) return;
  const auto centre = [&](const OverlayRect &rect) {
    return canvas.at(rect.x + rect.width / 2, rect.y + rect.height / 2);
  };
  expect(centre(filled[0]) == pip_color && centre(filled[1]) == pip_color,
         "filled pips paint the item color");
  expect(centre(empty[0]) != pip_color && canvas.at(empty[0].x, empty[0].y) == pip_color,
         "empty pips are one-pixel outlines in the item color");
  if (!expect(accent.has_value(), "an active indicator paints its accent in the on phase")) return;
  expect(canvas.at(accent->x, accent->y) == accent_color, "the accent lands on its pixels");
  const auto dark = painted(off);
  expect(std::ranges::none_of(off.ops, [](const OverlayOp &op) {
           return op.rgb == accent_color && op.kind == OverlayOpKind::fill_rect;
         }) && dark.at(accent->x, accent->y) != accent_color,
         "the off phase paints no accent and leaves no stale accent pixels");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(paints_fills_and_one_pixel_outlines_at_scale_one),
                 LAYERC_TEST_CASE(scales_logical_ops_to_device_pixels),
                 LAYERC_TEST_CASE(draws_centered_text_inside_its_rect),
                 LAYERC_TEST_CASE(paints_typed_pips_and_indicator_flash))
