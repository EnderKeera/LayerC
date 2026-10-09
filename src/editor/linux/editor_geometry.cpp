#include "editor_geometry.hpp"

#include <algorithm>
#include <cstdlib>

namespace layerc::editor {
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
