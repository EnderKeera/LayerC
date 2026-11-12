#include "editor_geometry.hpp"
#include "editor_style.hpp"

#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdlib>

namespace layerc::editor {
void EditorTabs::draw() {
  const bool show_focus = visible_focus() != 0;
  clear_visible_focus();
  Fl_Tabs::draw();
  if (show_focus) set_visible_focus();
  for (int index = 0; index < children(); ++index) {
    if (child(index) != value()) continue;
    const auto bounds = tab_bounds(index);
    if (!bounds) break;
    fl_push_clip(bounds->x, bounds->y, bounds->width, bounds->height);
    const auto accent = current_editor_tokens().accent;
    fl_color(fl_rgb_color(static_cast<uchar>(accent >> 16U),
                          static_cast<uchar>(accent >> 8U), static_cast<uchar>(accent)));
    fl_rectf(bounds->x, bounds->y, bounds->width, std::min(3, bounds->height));
    if (Fl::focus() == this && active_r() && show_focus && Fl::visible_focus())
      draw_editor_focus(bounds->x, bounds->y, bounds->width, bounds->height);
    fl_pop_clip();
    break;
  }
}

std::optional<EditorBounds> EditorTabs::tab_bounds(int index) {
  if (!visible_r() || index < 0 || index >= children() || w() <= 0 || h() <= 0)
    return std::nullopt;
  (void)tab_positions();
  const int header = tab_height();
  if (header == 0) return std::nullopt;
  const int left = std::max(x(), x() + tab_pos[index] + tab_offset);
  const int right = std::min(x() + w(), x() + tab_pos[index + 1] + tab_offset);
  const int height = std::min(h(), std::abs(header));
  if (right <= left || height <= 0) return std::nullopt;
  return EditorBounds{left, header > 0 ? y() : y() + h() - height, right - left, height};
}
} // namespace layerc::editor
