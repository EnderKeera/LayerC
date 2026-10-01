#include "display_types.hpp"

#include "test_support.hpp"

#include <string>

namespace {

using layerc::platform::OverlayDisplayList;
using layerc::platform::OverlayFrame;
using layerc::platform::OverlayOpKind;

[[nodiscard]] OverlayFrame card_frame() {
  OverlayFrame frame;
  frame.target = {100, 50, 800, 600};
  frame.panel.style = layerc::core::PanelStyle::card;
  frame.bars = {{"Q", 0.5, true}, {"", 0.0, false}};
  return frame;
}

} // namespace

void display_list_contracts() {
  using layerc::test::expect;

  const auto frame = card_frame();
  const auto list = layerc::platform::build_overlay_display_list(frame);
  expect(list.bounds == layerc::platform::panel_rect(frame),
         "display list bounds equal the panel rect");
  expect(list.ops.size() >= 2 &&
             list.ops[0].kind == OverlayOpKind::fill_rect &&
             list.ops[0].rgb == 0x272D35U &&
             list.ops[1].kind == OverlayOpKind::stroke_rect &&
             list.ops[1].rect.width == list.bounds.width,
         "card panels start with background fill and full border");
  std::size_t texts = 0;
  for (const auto &op : list.ops)
    if (op.kind == OverlayOpKind::text) {
      ++texts;
      expect(op.text == "Q" && op.font_px == 13 && op.rgb == 0xE4E8ECU,
             "card labels use the text colour at the 96 dpi font size");
    }
  expect(texts == 1, "empty labels paint no text op");

  auto hidden_text = frame;
  hidden_text.panel.style = layerc::core::PanelStyle::bar;
  hidden_text.panel.bar.show_text = false;
  for (const auto &op :
       layerc::platform::build_overlay_display_list(hidden_text).ops)
    expect(op.kind != OverlayOpKind::text,
           "bar panels without show_text paint no labels");

  auto scaled = frame;
  scaled.dpi = 192;
  const auto big = layerc::platform::build_overlay_display_list(scaled);
  expect(big.bounds.width >= list.bounds.width * 2 - 1,
         "192 dpi doubles the panel");

  std::string json;
  layerc::platform::append_overlay_display_list_json(
      json, OverlayDisplayList{{-5, 2, 3, 4},
                               {{OverlayOpKind::text, {0, 0, 3, 4}, 0x50BAC8U,
                                 "a\"b", 13}}});
  expect(json == "{\"x\":-5,\"y\":2,\"width\":3,\"height\":4,\"ops\":[{\"op\":"
                 "\"text\",\"x\":0,\"y\":0,\"width\":3,\"height\":4,\"color\":"
                 "\"#50BAC8\",\"text\":\"a\\\"b\",\"fontPx\":13}]}",
         "display list JSON is byte-stable");
}

void monitor_contracts() {
  using layerc::test::expect;
  layerc::platform::MonitorScan scan;
  expect(layerc::platform::monitor_dpi_for_rect(scan, 0, 0, 10, 10) == 96,
         "an empty scan yields the baseline");
  scan.monitors = {{"a", 0, 0, 100, 100, false, 96, 1},
                   {"b", 100, 0, 100, 100, true, 192, 2}};
  expect(layerc::platform::monitor_dpi_for_rect(scan, 120, 10, 50, 50) == 192,
         "largest overlap wins");
  expect(layerc::platform::monitor_dpi_for_rect(scan, 500, 500, 5, 5) == 192,
         "no overlap falls back to the primary");
  expect(layerc::platform::monitor_for_rect(scan, 10, 10, 5, 5)->name == "a",
         "monitor_for_rect returns the overlapping monitor");
  expect(layerc::platform::dpi_from_geometry(1920, 1080, 0, 0) == 96,
         "unknown physical size yields the baseline");
}

int main() {
  return layerc::test::run({{"display_list_contracts", display_list_contracts},
                            {"monitor_contracts", monitor_contracts}});
}
