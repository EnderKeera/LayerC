#include "layerc/core/panel_geometry.hpp"

#include <algorithm>

namespace layerc::core {

ItemPipLayout item_pip_layout(const PanelStyle style, const PanelRect &area,
                              const std::uint32_t pips) {
  ItemPipLayout layout{area, {}};
  const auto count = static_cast<int>(std::min(pips, max_counter_pips));
  if (count == 0 || area.width() <= 0 || area.height() <= 0) return layout;
  int side = std::min(8, area.height());
  int gap = 3;
  if (count * side + (count - 1) * gap > area.width()) {
    gap = 1;
    side = std::min(side, (area.width() - (count - 1) * gap) / count);
  }
  if (side < 2) return layout;
  const int width = count * side + (count - 1) * gap;
  int left = area.right - width;
  int top = area.top + (area.height() - side) / 2;
  if (style == PanelStyle::bar) {
    layout.text.right = std::max(area.left, left - gap);
  } else {
    left = area.left + (area.width() - width) / 2;
    top = area.bottom - side;
    layout.text.bottom = std::max(area.top, top - gap);
  }
  layout.pips.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const int x = left + index * (side + gap);
    layout.pips.push_back({x, top, x + side, top + side});
  }
  return layout;
}

} // namespace layerc::core
