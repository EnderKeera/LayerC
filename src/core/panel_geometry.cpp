#include "layerc/core/panel_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace layerc::core {

PanelGeometry panel_geometry(const Panel &panel, const std::size_t item_count,
                             const PanelMetrics metrics) {
  const auto sizing_count = std::max<std::size_t>(item_count, 1);
  PanelGeometry result;
  result.items.reserve(item_count);
  if (panel.style == PanelStyle::bar) {
    const auto width = static_cast<int>(panel.bar.width);
    const auto height = static_cast<int>(panel.bar.height);
    result.size = {metrics.padding * 2 + width,
                   metrics.padding * 2 +
                       static_cast<int>(sizing_count) * height +
                       static_cast<int>(sizing_count - 1) * metrics.spacing};
    for (std::size_t index = 0; index < item_count; ++index) {
      const int top = metrics.padding +
                      static_cast<int>(index) * (height + metrics.spacing);
      result.items.push_back(
          {metrics.padding, top, metrics.padding + width, top + height});
    }
    return result;
  }

  const auto extent = overlay_grid_extent(panel.layout, sizing_count);
  result.size = {
      metrics.padding * 2 +
          static_cast<int>(extent.columns) * metrics.card_item_width +
          static_cast<int>(extent.columns - 1) * metrics.spacing,
      metrics.padding * 2 +
          static_cast<int>(extent.rows) * metrics.card_item_height +
          static_cast<int>(extent.rows - 1) * metrics.spacing,
  };
  for (std::size_t index = 0; index < item_count; ++index) {
    std::size_t column = index;
    std::size_t row{};
    if (panel.layout == PanelLayout::column) {
      column = 0;
      row = index;
    } else if (panel.layout == PanelLayout::grid) {
      column = index % extent.columns;
      row = index / extent.columns;
    }
    const int left = metrics.padding +
                     static_cast<int>(column) *
                         (metrics.card_item_width + metrics.spacing);
    const int top = metrics.padding +
                    static_cast<int>(row) *
                        (metrics.card_item_height + metrics.spacing);
    result.items.push_back({left, top, left + metrics.card_item_width,
                            top + metrics.card_item_height});
  }
  return result;
}

BarGeometry bar_geometry(const Panel &panel, const std::size_t item_index,
                         const std::size_t item_count, const double progress,
                         const PanelMetrics metrics) {
  const auto layout = panel_geometry(panel, item_count, metrics);
  if (item_index >= layout.items.size()) return {};
  const auto track = layout.items[item_index];
  const auto bounded_progress = std::clamp(progress, 0.0, 1.0);
  const bool grouped = item_count > 1;
  BarGeometry result;
  result.grouped = grouped;
  result.corner_radius = metrics.corner_radius;
  result.fill_radius = metrics.fill_radius;
  if (!grouped) {
    const int visible_width = std::max(
        1, static_cast<int>(std::lround(track.width() * bounded_progress)));
    result.outline = {track.left, track.top, track.left + visible_width,
                      track.bottom};
    result.fill = result.outline;
  } else {
    result.outline = track;
    const int available = std::max(0, track.width() - metrics.bar_inset * 2);
    const int fill_width = static_cast<int>(
        std::lround(static_cast<double>(available) * bounded_progress));
    result.fill = {track.left + metrics.bar_inset,
                   track.top + metrics.bar_inset,
                   track.left + metrics.bar_inset + fill_width,
                   track.bottom - metrics.bar_inset};
  }
  result.text = inset_panel_rect(result.outline, 0);
  result.text.left += metrics.bar_text_inset_h;
  result.text.right -= metrics.bar_text_inset_h;
  result.text.top += metrics.bar_text_inset_v;
  result.text.bottom -= metrics.bar_text_inset_v;
  return result;
}

PanelRect inset_panel_rect(PanelRect rect, const int inset) noexcept {
  rect.left += inset;
  rect.top += inset;
  rect.right -= inset;
  rect.bottom -= inset;
  return rect;
}

} // namespace layerc::core
