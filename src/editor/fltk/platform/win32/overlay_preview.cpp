#include "platform/overlay_preview.hpp"

#include "layerc/core/panel_geometry.hpp"

#include <algorithm>

// Mirrors the daemon's GDI overlay (src/daemon/overlay_render.cpp,
// OverlayWindow::render and ensure_render_resources) at 96 dpi.
namespace layerc::editor {
namespace {

using core::ItemPresentation;
using core::PanelRect;
using Kind = OverlayPreviewOp::Kind;

// Segoe UI semibold at -MulDiv(14, 96, 72) pixels.
constexpr int overlay_font_px = 19;
// The daemon's pip corner, as a GDI ellipse diameter.
constexpr int pip_corner = 2;

// GDI RoundRect takes the corner ellipse's diameter, clamped to at least 1;
// the preview takes a radius.
[[nodiscard]] int corner_radius(const int diameter) noexcept {
  return std::max(1, std::max(1, diameter) / 2);
}

class Painter {
public:
  explicit Painter(std::vector<OverlayPreviewOp> &ops) : ops_(ops) {}
  void rect(const Kind kind, const PanelRect &rect, const int radius,
            const std::uint32_t rgb) {
    if (rect.width() <= 0 || rect.height() <= 0) return;
    ops_.push_back({kind, rect.left, rect.top, rect.width(), rect.height(),
                    radius, rgb, {}, 0});
  }
  // RoundRect with a brush and a pen: the fill, then its outline.
  void outlined(const PanelRect &shape, const int radius, const std::uint32_t fill,
                const std::uint32_t stroke) {
    rect(Kind::fill, shape, radius, fill);
    rect(Kind::stroke, shape, radius, stroke);
  }
  void text(const PanelRect &area, const std::string &label,
            const std::uint32_t rgb) {
    if (label.empty() || area.width() <= 0 || area.height() <= 0) return;
    ops_.push_back({Kind::text, area.left, area.top, area.width(), area.height(),
                    0, rgb, label, overlay_font_px});
  }

private:
  std::vector<OverlayPreviewOp> &ops_;
};

} // namespace

OverlayPreviewPaint
overlay_preview_paint(const core::Panel &panel,
                      const std::vector<core::ItemPresentation> &items) {
  using namespace core;
  OverlayPreviewPaint paint;
  paint.face = OverlayPreviewFace::interface_semibold;
  Painter painter(paint.ops);
  const auto &palette = overlay_palette;
  const bool is_bar = panel.style == PanelStyle::bar;
  const auto background = is_bar ? panel.bar.background_color : palette.card_background;
  const auto border = is_bar ? panel.bar.border_color : palette.card_border;
  const auto text_color = is_bar ? panel.bar.text_color : palette.card_text;
  const auto geometry = panel_geometry(panel, items.size());
  // A lone bar clips its window to the rounded bar, hiding the backdrop.
  if (!(is_bar && items.size() == 1))
    painter.rect(Kind::fill, {0, 0, geometry.size.width, geometry.size.height}, 0,
                 palette.window_backdrop);
  // Unlike the Linux overlay, unavailable rows keep their pips.
  const auto pips = [&](const ItemPipLayout &layout, const ItemPresentation &shown) {
    const int radius = corner_radius(pip_corner);
    for (std::size_t pip = 0; pip < layout.pips.size(); ++pip) {
      if (pip < shown.filled_pips) {
        painter.rect(Kind::fill, layout.pips[pip], radius, shown.color);
      } else {
        painter.outlined(layout.pips[pip], radius, background, shown.color);
      }
    }
  };
  for (std::size_t index = 0; index < items.size(); ++index) {
    const auto &shown = items[index];
    const bool indicator_on = shown.kind == OverlayItemKind::indicator &&
        !shown.unavailable && shown.active && shown.flash_on;
    const bool fill_visible = !shown.unavailable && (shown.has_progress || indicator_on);
    PanelRect text_area{};
    if (is_bar) {
      const auto shape = bar_geometry(panel, index, items.size(),
                                      shown.has_progress ? shown.remaining : 1.0);
      const int radius = corner_radius(shape.corner_radius);
      if (!shape.grouped) {
        painter.outlined(shape.outline, radius,
                         fill_visible ? shown.color : background, border);
      } else {
        painter.outlined(shape.outline, radius, background, border);
        if (fill_visible)
          painter.rect(Kind::fill, shape.fill, corner_radius(shape.fill_radius),
                       shown.color);
      }
      const auto layout = item_pip_layout(PanelStyle::bar, shape.text, shown.pips);
      pips(layout, shown);
      text_area = layout.text;
    } else {
      const auto &item = geometry.items[index];
      painter.outlined(item, corner_radius(overlay_metrics.fill_radius),
                       indicator_on ? shown.color : background, border);
      const auto layout = item_pip_layout(PanelStyle::card,
          inset_panel_rect(item, overlay_metrics.card_text_inset), shown.pips);
      pips(layout, shown);
      text_area = layout.text;
    }
    // The daemon gates card labels by the bar setting too.
    if (panel.bar.show_text) painter.text(text_area, shown.label, text_color);
  }
  return paint;
}

} // namespace layerc::editor
