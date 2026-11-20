#include "platform/overlay_preview.hpp"
#include "test_support.hpp"

#include "layerc/core/panel_geometry.hpp"
#ifndef _WIN32
#include "display_types.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// The editor preview paints what the local overlay paints: Windows mirrors
// the daemon's GDI overlay, Linux takes the shared display list unchanged.
namespace {
using namespace layerc;
using editor::OverlayPreviewFace;
using editor::OverlayPreviewOp;
using editor::overlay_preview_paint;
using test::expect;
using Kind = OverlayPreviewOp::Kind;

constexpr std::uint32_t background = 0x101010U;
constexpr std::uint32_t border = 0x202020U;
constexpr std::uint32_t text_color = 0x303030U;

core::Panel bar_panel() {
  core::Panel panel;
  panel.id = "main";
  panel.style = core::PanelStyle::bar;
  panel.bar.background_color = background;
  panel.bar.border_color = border;
  panel.bar.text_color = text_color;
  return panel;
}

core::ItemPresentation row(const std::string &label, const std::uint32_t color) {
  core::ItemPresentation item;
  item.item_id = label;
  item.label = label;
  item.color = color;
  item.remaining = 0.5;
  item.ready = false;
  return item;
}

#ifdef _WIN32
std::size_t count_if(const std::vector<OverlayPreviewOp> &ops,
                     const Kind kind, const std::uint32_t rgb) {
  return static_cast<std::size_t>(std::ranges::count_if(ops, [&](const auto &op) {
    return op.kind == kind && op.rgb == rgb;
  }));
}

constexpr std::uint32_t backdrop = 0x151A20U;

void lone_bar_has_rounded_shape_and_no_backdrop() {
  const auto paint = overlay_preview_paint(bar_panel(), {row("Dash", 0xAA0000U)});
  expect(paint.face == OverlayPreviewFace::interface_semibold, "Segoe UI semibold face");
  expect(count_if(paint.ops, Kind::fill, backdrop) == 0,
         "a lone bar's window region hides the backdrop");
  if (!expect(paint.ops.size() >= 3, "fill, outline and label")) return;
  const auto shape = core::bar_geometry(bar_panel(), 0, 1, 0.5);
  const int radius = core::overlay_metrics.corner_radius / 2;
  const auto &fill = paint.ops[0];
  expect(fill.kind == Kind::fill && fill.rgb == 0xAA0000U, "progress fills the bar");
  expect(fill.x == shape.outline.left && fill.width == shape.outline.width(),
         "the outline follows the remaining fraction");
  expect(fill.radius == radius, "GDI ellipse diameter becomes a radius");
  const auto &outline = paint.ops[1];
  expect(outline.kind == Kind::stroke && outline.rgb == border && outline.radius == radius,
         "rounded border");
  const auto &label = paint.ops.back();
  expect(label.kind == Kind::text && label.text == "Dash" && label.rgb == text_color &&
             label.font_px == 19,
         "label at the daemon's 96 dpi font height");
}

void grouped_bars_have_backdrop_and_inset_fill() {
  const auto panel = bar_panel();
  const auto paint = overlay_preview_paint(panel, {row("A", 0xAA0000U), row("B", 0x00AA00U)});
  const auto geometry = core::panel_geometry(panel, 2);
  if (!expect(!paint.ops.empty(), "ops painted")) return;
  const auto &first = paint.ops.front();
  expect(first.kind == Kind::fill && first.rgb == backdrop && first.radius == 0 &&
             first.x == 0 && first.y == 0 && first.width == geometry.size.width &&
             first.height == geometry.size.height,
         "the window backdrop is painted first");
  expect(count_if(paint.ops, Kind::fill, background) == 2, "each track is filled");
  expect(count_if(paint.ops, Kind::stroke, border) == 2, "each track is outlined");
  const auto fill = std::ranges::find_if(paint.ops, [](const auto &op) {
    return op.kind == Kind::fill && op.rgb == 0xAA0000U;
  });
  if (!expect(fill != paint.ops.end(), "progress fill")) return;
  const auto shape = core::bar_geometry(panel, 0, 2, 0.5);
  expect(fill->x == shape.fill.left && fill->width == shape.fill.width() &&
             fill->radius == core::overlay_metrics.fill_radius / 2,
         "inset rounded fill without an outline");
  expect(count_if(paint.ops, Kind::stroke, 0xAA0000U) == 0, "fills have no pen");
}

void cards_use_the_daemon_palette() {
  core::Panel panel;
  panel.id = "cards";
  const auto paint = overlay_preview_paint(panel, {row("Card", 0xAA0000U)});
  expect(count_if(paint.ops, Kind::fill, backdrop) == 1, "card backdrop");
  expect(count_if(paint.ops, Kind::fill, 0x272D35U) == 1, "card fill");
  expect(count_if(paint.ops, Kind::stroke, 0x50BAC8U) == 1, "card border");
  expect(count_if(paint.ops, Kind::text, 0xE4E8ECU) == 1, "card label");
  const auto card = std::ranges::find_if(paint.ops, [](const auto &op) {
    return op.kind == Kind::fill && op.rgb == 0x272D35U;
  });
  expect(card != paint.ops.end() && card->radius == core::overlay_metrics.fill_radius / 2,
         "rounded card");
}

void unavailable_rows_keep_their_pips() {
  auto item = row("Charges", 0x0000AAU);
  item.unavailable = true;
  item.has_progress = false;
  item.pips = 3;
  item.filled_pips = 1;
  const auto paint = overlay_preview_paint(bar_panel(), {item});
  expect(count_if(paint.ops, Kind::fill, 0x0000AAU) == 1, "one filled pip, no bar fill");
  expect(count_if(paint.ops, Kind::stroke, 0x0000AAU) == 2, "two outlined pips");
  expect(std::ranges::count_if(paint.ops, [](const auto &op) { return op.radius == 1; }) == 5,
         "pips keep the daemon's small corner");
}

void hidden_text_paints_no_labels() {
  auto panel = bar_panel();
  panel.bar.show_text = false;
  const auto paint = overlay_preview_paint(panel, {row("A", 1), row("B", 2)});
  expect(std::ranges::none_of(paint.ops, [](const auto &op) { return op.kind == Kind::text; }),
         "labels follow the bar's text setting");
}
#else
void matches_the_shared_display_list(const core::Panel &panel,
                                     const std::vector<core::ItemPresentation> &items) {
  platform::OverlayFrame frame;
  frame.panel = panel;
  for (const auto &item : items)
    frame.bars.push_back(platform::overlay_bar_from_presentation(item));
  const auto display = platform::build_overlay_display_list(frame);
  const auto paint = overlay_preview_paint(panel, items);
  expect(paint.face == OverlayPreviewFace::monospace, "the overlay's monospace face");
  if (!expect(paint.ops.size() == display.ops.size(), "one preview op per display op"))
    return;
  for (std::size_t index = 0; index < paint.ops.size(); ++index) {
    const auto &got = paint.ops[index];
    const auto &want = display.ops[index];
    const auto kind = want.kind == platform::OverlayOpKind::fill_rect ? Kind::fill
        : want.kind == platform::OverlayOpKind::stroke_rect ? Kind::stroke : Kind::text;
    expect(got.kind == kind && got.x == want.rect.x && got.y == want.rect.y &&
               got.width == want.rect.width && got.height == want.rect.height &&
               got.rgb == want.rgb && got.text == want.text &&
               got.font_px == want.font_px && got.radius == 0,
           "op " + std::to_string(index) + " is the display list's, square");
  }
}

void linux_preview_is_the_display_list() {
  auto unavailable = row("Charges", 0x0000AAU);
  unavailable.unavailable = true;
  unavailable.has_progress = false;
  unavailable.pips = 3;
  unavailable.filled_pips = 1;
  matches_the_shared_display_list(bar_panel(), {row("Dash", 0xAA0000U)});
  matches_the_shared_display_list(bar_panel(), {row("A", 0xAA0000U), unavailable});
  core::Panel cards;
  cards.id = "cards";
  matches_the_shared_display_list(cards, {row("Card", 0xAA0000U), row("Two", 0x00AA00U)});
}
#endif

} // namespace

#ifdef _WIN32
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(lone_bar_has_rounded_shape_and_no_backdrop),
                 LAYERC_TEST_CASE(grouped_bars_have_backdrop_and_inset_fill),
                 LAYERC_TEST_CASE(cards_use_the_daemon_palette),
                 LAYERC_TEST_CASE(unavailable_rows_keep_their_pips),
                 LAYERC_TEST_CASE(hidden_text_paints_no_labels))
#else
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(linux_preview_is_the_display_list))
#endif
