#include "editor_scroll.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <algorithm>

namespace layerc::editor {
namespace {
EditorBounds intersect(EditorBounds a, const EditorBounds &b) {
  const int right = std::min(a.x + a.width, b.x + b.width);
  const int bottom = std::min(a.y + a.height, b.y + b.height);
  a.x = std::max(a.x, b.x); a.y = std::max(a.y, b.y);
  a.width = std::max(0, right - a.x); a.height = std::max(0, bottom - a.y);
  return a;
}
EditorBounds scroll_viewport(const Fl_Scroll &scroll) {
  if (const auto *host = dynamic_cast<const EditorScroll *>(&scroll)) return host->viewport();
  EditorBounds bounds{scroll.x() + Fl::box_dx(scroll.box()),
                      scroll.y() + Fl::box_dy(scroll.box()),
                      scroll.w() - Fl::box_dw(scroll.box()),
                      scroll.h() - Fl::box_dh(scroll.box())};
  if (scroll.scrollbar.visible()) {
    bounds.width -= scroll.scrollbar.w();
    if (scroll.scrollbar.align() & FL_ALIGN_LEFT) bounds.x += scroll.scrollbar.w();
  }
  if (scroll.hscrollbar.visible()) {
    bounds.height -= scroll.hscrollbar.h();
    if (scroll.scrollbar.align() & FL_ALIGN_TOP) bounds.y += scroll.hscrollbar.h();
  }
  return bounds;
}
}
EditorScroll::EditorScroll(int x, int y, int width, int height, const char *label)
    : Fl_Scroll(x, y, width, height, label) { Fl::add_check(follow_focus, this); }
EditorScroll::~EditorScroll() { Fl::remove_check(follow_focus, this); }
EditorBounds EditorScroll::viewport() const {
  EditorBounds bounds;
  bbox(bounds.x, bounds.y, bounds.width, bounds.height);
  return bounds;
}
void EditorScroll::follow_focus(void *data) {
  auto &self = *static_cast<EditorScroll *>(data);
  auto *focus = Fl::focus();
  if (self.focus_ && self.focus_->widget() == focus) return;
  self.focus_ = focus ? std::make_unique<Fl_Widget_Tracker>(focus) : nullptr;
  if (focus && self.contains(focus) && focus != &self &&
      focus != &self.scrollbar && focus != &self.hscrollbar) self.reveal(*focus);
}
void EditorScroll::reveal(Fl_Widget &widget) {
  if (!widget.visible_r() || !contains(&widget)) return;
  const auto view = viewport();
  const auto shift = [](int position, int size, int start, int available) {
    if (position < start || size > available) return position - start;
    return std::max(0, position + size - start - available);
  };
  scroll_to(xposition() + shift(widget.x(), widget.w(), view.x, view.width),
            yposition() + shift(widget.y(), widget.h(), view.y, view.height));
}
std::optional<EditorBounds> visible_bounds(const Fl_Widget &widget, EditorBounds bounds) {
  if (!widget.visible_r()) return std::nullopt;
  const Fl_Widget *child = &widget;
  for (auto *parent = widget.parent(); parent; child = parent, parent = parent->parent()) {
    if (const auto *scroll = dynamic_cast<const Fl_Scroll *>(parent);
        scroll && child != &scroll->scrollbar && child != &scroll->hscrollbar)
      bounds = intersect(bounds, scroll_viewport(*scroll));
    if (const auto *window = dynamic_cast<const Fl_Window *>(parent))
      bounds = intersect(bounds, {0, 0, window->w(), window->h()});
  }
  if (bounds.width <= 0 || bounds.height <= 0) return std::nullopt;
  return bounds;
}
std::optional<EditorBounds> visible_bounds(const Fl_Widget &widget) {
  return visible_bounds(widget, {widget.x(), widget.y(), widget.w(), widget.h()});
}
} // namespace layerc::editor
