#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <vector>

namespace layerc::core {

struct PanelSize {
  int width{};
  int height{};

  bool operator==(const PanelSize &) const = default;
};

struct PanelRect {
  int left{};
  int top{};
  int right{};
  int bottom{};

  [[nodiscard]] int width() const noexcept { return right - left; }
  [[nodiscard]] int height() const noexcept { return bottom - top; }
  bool operator==(const PanelRect &) const = default;
};

struct PanelGeometry {
  PanelSize size;
  std::vector<PanelRect> items;
};

struct BarGeometry {
  PanelRect outline;
  PanelRect fill;
  PanelRect text;
  int corner_radius{};
  int fill_radius{};
  bool grouped{};
};

// Geometry is expressed in profile DIPs. Hosts apply their one scale when
// converting the returned sizes and rectangles to device pixels.
[[nodiscard]] PanelGeometry
panel_geometry(const Panel &panel, std::size_t item_count,
               PanelMetrics metrics = overlay_metrics);
[[nodiscard]] BarGeometry
bar_geometry(const Panel &panel, std::size_t item_index,
             std::size_t item_count, double progress,
             PanelMetrics metrics = overlay_metrics);
[[nodiscard]] PanelRect inset_panel_rect(PanelRect rect, int inset) noexcept;

} // namespace layerc::core
