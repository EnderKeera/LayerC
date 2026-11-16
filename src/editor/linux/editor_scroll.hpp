#pragma once

#include "editor_geometry.hpp"
#include <FL/Fl_Scroll.H>
#include <memory>
#include <optional>

namespace layerc::editor {

// One focus-following host for authored forms. Checks run only when FLTK is
// about to block, including focus changes made by accessibility actions.
class EditorScroll final : public Fl_Scroll {
public:
  EditorScroll(int x, int y, int width, int height, const char *label = nullptr);
  ~EditorScroll() override;
  [[nodiscard]] EditorBounds viewport() const;
  void reveal(Fl_Widget &widget);
private:
  std::unique_ptr<Fl_Widget_Tracker> focus_;
  static void follow_focus(void *self);
};

// Geometry in the widget's window coordinates, clipped by ancestor scroll
// viewports and the window. Hidden or completely clipped widgets have no bounds.
[[nodiscard]] std::optional<EditorBounds> visible_bounds(const Fl_Widget &widget);
[[nodiscard]] std::optional<EditorBounds> visible_bounds(const Fl_Widget &widget,
                                                         EditorBounds bounds);
} // namespace layerc::editor
