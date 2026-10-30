#include "layerc/core/panel_geometry.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>

namespace layerc::test {
void test_item_pip_geometry() {
  using namespace layerc::core;
  const auto inside = [](const PanelRect &outer, const PanelRect &inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom &&
           inner.width() >= 0 && inner.height() >= 0;
  };
  for (const auto style : {PanelStyle::bar, PanelStyle::card}) {
    for (const auto area : std::array{PanelRect{10, 20, 50, 36},
          PanelRect{10, 20, 250, 52}, PanelRect{10, 20, 2058, 276},
          PanelRect{8, 8, 168, 64}}) {
      int previous_side = 9;
      for (std::uint32_t count = 1; count <= max_counter_pips; ++count) {
        const auto layout = item_pip_layout(style, area, count);
        expect(layout.pips.size() == count && inside(area, layout.text),
               "pip layout retains every pip and bounds the text");
        if (layout.pips.empty()) continue;
        const int side = layout.pips.front().width();
        expect(side >= 2 && side <= previous_side, "pip size shrinks monotonically as count grows");
        previous_side = side;
        for (std::size_t index = 0; index < layout.pips.size(); ++index) {
          const auto &pip = layout.pips[index];
          expect(inside(area, pip) && pip.width() == pip.height(), "each pip is a bounded square");
          if (index) expect(layout.pips[index - 1].right < pip.left, "pips never overlap");
          if (style == PanelStyle::bar)
            expect(layout.text.right <= pip.left, "bar text excludes the pip block");
          else expect(layout.text.bottom <= pip.top, "card text excludes the bottom pip strip");
        }
      }
    }
  }
  const PanelRect tiny{0, 0, 5, 1};
  const auto omitted = item_pip_layout(PanelStyle::bar, tiny, max_counter_pips);
  expect(omitted.pips.empty() && omitted.text == tiny, "tiny layouts omit pips without stealing text");
  const auto none = item_pip_layout(PanelStyle::bar, tiny, 0);
  expect(none.pips.empty() && none.text == tiny, "zero pips preserve the complete text area");
}
} // namespace layerc::test
