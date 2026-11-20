#include "platform/overlay_preview.hpp"

#include "display_types.hpp"

#include <utility>

namespace layerc::editor {

// The X11, cairo and GNOME overlays all paint this display list, so the
// preview takes it unchanged: square operations in the monospace face.
OverlayPreviewPaint
overlay_preview_paint(const core::Panel &panel,
                      const std::vector<core::ItemPresentation> &items) {
  platform::OverlayFrame frame;
  frame.panel = panel;
  frame.bars.reserve(items.size());
  for (const auto &item : items)
    frame.bars.push_back(platform::overlay_bar_from_presentation(item));
  const auto display = platform::build_overlay_display_list(frame);
  OverlayPreviewPaint paint;
  paint.ops.reserve(display.ops.size());
  for (const auto &op : display.ops) {
    OverlayPreviewOp out;
    switch (op.kind) {
    case platform::OverlayOpKind::fill_rect: out.kind = OverlayPreviewOp::Kind::fill; break;
    case platform::OverlayOpKind::stroke_rect: out.kind = OverlayPreviewOp::Kind::stroke; break;
    case platform::OverlayOpKind::text: out.kind = OverlayPreviewOp::Kind::text; break;
    }
    out.x = op.rect.x;
    out.y = op.rect.y;
    out.width = op.rect.width;
    out.height = op.rect.height;
    out.rgb = op.rgb;
    out.text = op.text;
    out.font_px = op.font_px;
    paint.ops.push_back(std::move(out));
  }
  return paint;
}

} // namespace layerc::editor
