#pragma once

#include <FL/Fl_Tabs.H>
#include <optional>

namespace layerc::editor {
// FLTK logical coordinates, relative to the owning native window. Exporters
// still clip ancestors and convert to the requested device coordinate space.
struct EditorBounds {
  int x{}, y{}, width{}, height{};
  [[nodiscard]] bool contains(int px, int py) const {
    return px >= x && py >= y && px - x < width && py - y < height;
  }
};

// Reuse FLTK's measured/compressed tab positions, not the page's large content
// rectangle or a second font-layout implementation.
class EditorTabs final : public Fl_Tabs {
public:
  using Fl_Tabs::Fl_Tabs;
  [[nodiscard]] std::optional<EditorBounds> tab_bounds(int index);
protected:
  void draw() override;
};
} // namespace layerc::editor
