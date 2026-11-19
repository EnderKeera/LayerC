#include "editor_popup.hpp"
#include "a11y/fltk_hooks.hpp"
#include "editor_style.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Menu_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <memory>
#include <vector>

namespace layerc::editor {
namespace {
std::vector<int> level_items(const Fl_Menu_ &owner, int first) {
  std::vector<int> result;
  const auto *array = owner.menu();
  if (!array || first < 0 || first >= owner.size() - 1) return result;
  for (auto *item = array + first; item->label(); item = item->next())
    if (item->visible()) result.push_back(static_cast<int>(item - array));
  return result;
}
std::optional<EditorBounds> title_bounds(const Fl_Menu_ &owner, int index) {
  if (!dynamic_cast<const Fl_Menu_Bar *>(&owner) || !owner.visible_r()) return {};
  int x = owner.x() + 6;
  for (const int item : level_items(owner, 0)) {
    const int width = owner.menu()[item].measure(nullptr, &owner) + 16;
    if (item == index) return EditorBounds{x, owner.y(), width, owner.h()};
    x += width;
  }
  return {};
}
bool contains_point(const EditorBounds &b, int x, int y) {
  return x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height;
}
class PopupSession;
PopupSession *tracking{}; // FLTK's single UI thread; lifetime is one tracking loop.
class PopupWindow final : public Fl_Menu_Window {
public:
  PopupWindow(PopupSession &session, int first, int x, int y, int minimum_width);
  std::optional<EditorBounds> bounds(int index) const;
  void reveal(int index);
  int handle(int event) override;
  std::vector<int> items;
  int selected{-1};
  int pitch{}, offset{};
private:
  PopupSession &session_;
  void draw() override;
};
class PopupSession final {
public:
  explicit PopupSession(Fl_Menu_ &menu) : owner(menu), tracker(&menu), array(menu.menu()),
      bar(dynamic_cast<Fl_Menu_Bar *>(&menu) != nullptr) {}
  Fl_Menu_ &owner;
  Fl_Widget_Tracker tracker;
  const Fl_Menu_Item *array;
  bool bar{}, done{}, pushed{};
  int current{-1}, title{-1}, picked{-1};
  std::vector<std::unique_ptr<PopupWindow>> windows;
  void highlight(int level, int index);
  void move(int direction, bool edge = false);
  void switch_title(int direction);
  void choose(int index);
  void finish() {
    done = true;
    // Also end the native modal wait when an AT-SPI deferred action (rather
    // than an X event) closes the popup. Keep the handling window alive.
    for (auto &window : windows) window->hide();
  }
  int handle(int event);
  void start(int initial_title);
};
PopupWindow::PopupWindow(PopupSession &session, int first, int x, int y, int minimum_width)
    : Fl_Menu_Window(x, y, 1, 1), items(level_items(session.owner, first)), session_(session) {
  end();
  set_menu_window(); set_modal(); clear_border();
  box(session.owner.menu_box() == FL_NO_BOX ? FL_UP_BOX : session.owner.menu_box());
  color(session.owner.color());
  int width = minimum_width;
  for (const int index : items) {
    const auto &item = session.array[index];
    int height{};
    int measured = item.measure(&height, &session.owner);
    pitch = std::max(pitch, height + Fl::menu_linespacing());
    if (item.submenu()) measured += FL_NORMAL_SIZE;
    else if (item.shortcut()) measured += static_cast<int>(fl_width(fl_shortcut_label(item.shortcut()))) + 16;
    width = std::max(width, measured + 2 * Fl::box_dx(box()) + 7);
  }
  pitch = std::max(1, pitch);
  int sx{}, sy{}, sw{}, sh{};
  Fl::screen_work_area(sx, sy, sw, sh, x, y);
  width = std::min(width, sw);
  const int height = std::min(static_cast<int>(items.size()) * pitch + 2 * Fl::box_dy(box()), sh);
  resize(std::clamp(x, sx, sx + sw - width), std::clamp(y, sy, sy + sh - height), width, height);
}
std::optional<EditorBounds> PopupWindow::bounds(int index) const {
  const auto found = std::find(items.begin(), items.end(), index);
  if (found == items.end()) return {};
  const int top = Fl::box_dy(box()) + static_cast<int>(found - items.begin()) * pitch - offset;
  const int visible_top = std::max(Fl::box_dy(box()), top);
  const int bottom = std::min(h() - Fl::box_dy(box()), top + pitch);
  if (bottom <= visible_top) return {};
  return EditorBounds{x() + Fl::box_dx(box()), y() + visible_top,
                      w() - Fl::box_dw(box()), bottom - visible_top};
}
void PopupWindow::reveal(int index) {
  const auto found = std::find(items.begin(), items.end(), index);
  if (found == items.end()) return;
  const int top = static_cast<int>(found - items.begin()) * pitch;
  const int available = h() - Fl::box_dh(box());
  if (top < offset) offset = top;
  else if (top + pitch > offset + available) offset = top + pitch - available;
  redraw();
}
void PopupWindow::draw() {
  fl_draw_box(box(), 0, 0, w(), h(), color());
  fl_push_clip(Fl::box_dx(box()), Fl::box_dy(box()), w() - Fl::box_dw(box()), h() - Fl::box_dh(box()));
  for (std::size_t row = 0; row < items.size(); ++row) {
    const auto &item = session_.array[items[row]];
    const int x = Fl::box_dx(box());
    const int y = Fl::box_dy(box()) + static_cast<int>(row) * pitch - offset + Fl::menu_linespacing() / 2;
    const int width = w() - Fl::box_dw(box()) - 1;
    const int height = pitch - Fl::menu_linespacing();
    item.draw(x, y, width, height, &session_.owner, items[row] == selected);
    // Fl_Menu_Item::draw supplies the native label/check/radio painting; the
    // menu window supplies shortcuts and submenu arrows, just as FLTK does.
    if (item.submenu()) {
      fl_draw_arrow(Fl_Rect(x + width - 15, y + (height - 13) / 2, 13, 13),
                    FL_ARROW_SINGLE, FL_ORIENT_RIGHT, fl_color());
    } else if (item.shortcut()) {
      fl_draw(fl_shortcut_label(item.shortcut()), x, y, width - 4, height, FL_ALIGN_RIGHT);
    }
    if (item.flags & FL_MENU_DIVIDER) {
      const auto divider = current_editor_tokens().divider;
      fl_color(fl_rgb_color(static_cast<uchar>(divider >> 16U),
                             static_cast<uchar>(divider >> 8U), static_cast<uchar>(divider)));
      fl_xyline(x, y + height, x + width);
    }
  }
  fl_pop_clip();
}
int PopupWindow::handle(int event) { return session_.handle(event); }
void PopupSession::highlight(int level, int index) {
  if (level < 0) {
    if (index == title) { current = index; return; }
    windows.clear(); title = current = index;
    owner.redraw();
    if (index < 0 || !array[index].activevisible() || !(array[index].flags & FL_SUBMENU)) return;
    const auto bounds = title_bounds(owner, index);
    if (!bounds) return;
    windows.push_back(std::make_unique<PopupWindow>(*this, index + 1,
        owner.top_window()->x_root() + bounds->x, owner.top_window()->y_root() + bounds->y + bounds->height, 0));
  } else {
    auto &window = *windows[static_cast<std::size_t>(level)];
    current = index;
    window.selected = index >= 0 && array[index].activevisible() ? index : -1;
    window.reveal(index); window.redraw();
    // Keep the submenu only when it still belongs to the same row.
    const auto keep = static_cast<std::size_t>(level + 1);
    if (windows.size() > keep && window.selected == index && index >= 0 &&
        (array[index].flags & FL_SUBMENU) && windows[keep]->items == level_items(owner, index + 1)) return;
    windows.resize(keep);
    if (index >= 0 && array[index].activevisible() && (array[index].flags & FL_SUBMENU)) {
      const auto bounds = window.bounds(index);
      if (bounds) windows.push_back(std::make_unique<PopupWindow>(*this, index + 1,
          window.x() + window.w(), bounds->y, 0));
    }
  }
  for (auto &window : windows) if (!window->shown()) window->show();
  if (!windows.empty()) Fl::grab(windows.front().get());
}
void PopupSession::move(int direction, bool edge) {
  if (windows.empty()) return;
  std::size_t level = windows.size() - 1;
  // A newly opened submenu has no current child yet.
  auto &window = *windows[level];
  const auto found = std::find(window.items.begin(), window.items.end(), current);
  int row = found == window.items.end() ? (direction > 0 ? -1 : static_cast<int>(window.items.size()))
                                      : static_cast<int>(found - window.items.begin());
  if (edge) row = direction > 0 ? -1 : static_cast<int>(window.items.size());
  for (std::size_t attempt = 0; attempt < window.items.size(); ++attempt) {
    row = (row + direction + static_cast<int>(window.items.size())) % static_cast<int>(window.items.size());
    const int index = window.items[static_cast<std::size_t>(row)];
    if (array[index].activevisible()) { highlight(static_cast<int>(level), index); return; }
  }
}
void PopupSession::switch_title(int direction) {
  const auto titles = level_items(owner, 0);
  if (titles.empty()) return;
  const auto found = std::find(titles.begin(), titles.end(), title);
  int row = found == titles.end() ? 0 : static_cast<int>(found - titles.begin());
  for (std::size_t attempt = 0; attempt < titles.size(); ++attempt) {
    row = (row + direction + static_cast<int>(titles.size())) % static_cast<int>(titles.size());
    const int index = titles[static_cast<std::size_t>(row)];
    if (array[index].activevisible()) { highlight(-1, index); return; }
  }
}
void PopupSession::choose(int index) {
  if (index < 0 || index >= owner.size() - 1 || !array[index].activevisible()) return;
  if (array[index].submenu()) {
    if (bar && title_bounds(owner, index)) highlight(-1, index);
    else for (std::size_t level = 0; level < windows.size(); ++level)
      if (std::find(windows[level]->items.begin(), windows[level]->items.end(), index) != windows[level]->items.end()) {
        highlight(static_cast<int>(level), index); break;
      }
    move(1, true); return;
  }
  picked = index; finish();
}
int PopupSession::handle(int event) {
  if (tracker.deleted() || owner.menu() != array) { finish(); return 1; }
  switch (event) {
  case FL_KEYBOARD:
    switch (Fl::event_key()) {
    case FL_Escape: finish(); return 1;
    case FL_Up: move(-1); return 1;
    case FL_Down: move(1); return 1;
    case FL_Home: move(1, true); return 1;
    case FL_End: move(-1, true); return 1;
    case FL_Tab: move(Fl::event_shift() ? -1 : 1); return 1;
    case FL_Right:
      if (windows.size() > 1) move(1, true);
      else if (bar) switch_title(1);
      return 1;
    case FL_Left:
      if (windows.size() > 1) {
        windows.pop_back(); current = windows.back()->selected;
      } else if (bar) switch_title(-1);
      return 1;
    case FL_Enter: case FL_KP_Enter: case ' ': choose(current); return 1;
    default: break;
    }
    break;
  case FL_SHORTCUT:
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
      if (!(*it)->items.empty())
        if (const auto *item = (array + (*it)->items.front())->find_shortcut()) {
          const int index = static_cast<int>(item - array);
          highlight(static_cast<int>(windows.rend() - it - 1), index);
          choose(index); return 1;
        }
    break;
  case FL_MOUSEWHEEL:
    if (!windows.empty()) {
      auto &window = *windows.back();
      const int maximum = std::max(0, static_cast<int>(window.items.size()) * window.pitch - window.h() + Fl::box_dh(window.box()));
      window.offset = std::clamp(window.offset + Fl::event_dy() * window.pitch, 0, maximum);
      window.redraw();
    }
    return 1;
  case FL_ENTER: case FL_MOVE: case FL_DRAG: case FL_PUSH: {
    const int x = Fl::event_x_root(), y = Fl::event_y_root();
    if (bar) for (const int index : level_items(owner, 0)) {
      auto bounds = title_bounds(owner, index);
      if (bounds) {
        bounds->x += owner.top_window()->x_root(); bounds->y += owner.top_window()->y_root();
        if (contains_point(*bounds, x, y)) { highlight(-1, index); return 1; }
      }
    }
    for (std::size_t level = windows.size(); level-- > 0;)
      for (const int index : windows[level]->items)
        if (const auto bounds = windows[level]->bounds(index); bounds && contains_point(*bounds, x, y)) {
          highlight(static_cast<int>(level), index);
          if (event == FL_PUSH) pushed = true;
          return 1;
        }
    if (event == FL_PUSH) finish();
    else if (!windows.empty()) { windows.back()->selected = -1; current = -1; windows.back()->redraw(); }
    return 1;
  }
  case FL_RELEASE:
    if (pushed || !Fl::event_is_click()) choose(current);
    return 1;
  case FL_FOCUS: case FL_UNFOCUS: return 1;
  default: break;
  }
  return 0;
}
void PopupSession::start(int initial_title) {
  if (bar) { highlight(-1, initial_title); return; }
  auto *window = owner.top_window();
  const bool choice = dynamic_cast<Fl_Choice *>(&owner) != nullptr;
  const int x = choice ? window->x_root() + owner.x() : Fl::event_x_root();
  const int y = choice ? window->y_root() + owner.y() : Fl::event_y_root();
  windows.push_back(std::make_unique<PopupWindow>(*this, 0, x, y, choice ? owner.w() : 0));
  if (choice && owner.mvalue()) {
    current = static_cast<int>(owner.mvalue() - array);
    windows.front()->selected = current;
    windows.front()->reveal(current);
  }
  Fl::grab(windows.front().get());
  windows.front()->show();
}
}
void track_editor_popup(Fl_Menu_ &owner, int title) {
  if (tracking || !owner.menu() || !owner.menu()->label() || !owner.active_r() || !owner.top_window()) return;
  auto *group = Fl_Group::current(); Fl_Group::current(nullptr);
  PopupSession session(owner);
  Fl_Widget_Tracker focus(Fl::focus());
  tracking = &session;
  session.start(title);
  while (!session.done && !session.windows.empty() && !session.tracker.deleted() &&
         owner.menu() == session.array && owner.visible_r() && owner.top_window()->shown()) Fl::wait();
  session.windows.clear();
  Fl::grab(nullptr);
  tracking = nullptr;
  Fl_Group::current(group);
  if (auto *previous = focus.widget(); previous && previous->visible_r() && previous->active_r() &&
      (!Fl::modal() || previous->top_window() == Fl::modal())) previous->take_focus();
  if (session.tracker.deleted()) return;
  owner.redraw();
  if (session.picked >= 0 && owner.menu() == session.array) (void)owner.picked(session.array + session.picked);
}
Fl_Menu_ *popup_owner() { return tracking && !tracking->tracker.deleted() ? &tracking->owner : nullptr; }
bool popup_current(const Fl_Menu_ &owner, int index) {
  return popup_owner() == &owner && tracking->current == index && owner.menu()[index].activevisible();
}
std::optional<EditorBounds> popup_item_bounds(const Fl_Menu_ &owner, int index) {
  if (auto bounds = title_bounds(owner, index)) return bounds;
  if (popup_owner() != &owner) return {};
  for (const auto &window : tracking->windows)
    if (auto bounds = window->bounds(index)) {
      bounds->x -= owner.top_window()->x_root(); bounds->y -= owner.top_window()->y_root();
      return bounds;
    }
  return {};
}
Fl_Window *popup_document_window(const Fl_Window *transient) {
  if (popup_owner()) for (const auto &window : tracking->windows)
    if (window.get() == transient) return tracking->owner.top_window();
  return nullptr;
}
bool popup_pick(Fl_Menu_ &owner, int index) {
  if (popup_owner() != &owner) return false;
  tracking->choose(index); return true;
}
int EditorMenuBar::handle(int event) {
  if (event == FL_PUSH) {
    for (const int index : level_items(*this, 0))
      if (const auto bounds = title_bounds(*this, index); bounds && contains_point(*bounds, Fl::event_x(), Fl::event_y())) {
        track_editor_popup(*this, index); return 1;
      }
    return 1;
  }
  if (event == FL_SHORTCUT && visible_r() && menu()) {
    if (const auto *item = menu()->find_shortcut(nullptr, true); item && item->submenu()) {
      track_editor_popup(*this, static_cast<int>(item - menu())); return 1;
    }
    return test_shortcut() != nullptr;
  }
  return event == FL_ENTER || event == FL_LEAVE;
}
void EditorMenuBar::draw() {
  draw_box();
  for (const int index : level_items(*this, 0))
    if (const auto bounds = title_bounds(*this, index))
      menu()[index].draw(bounds->x, bounds->y, bounds->width, bounds->height, this,
                         popup_owner() == this && tracking->title == index ? 2 : 0);
}
int EditorChoice::handle(int event) {
  if (menu() && menu()->label() &&
      (event == FL_PUSH || (event == FL_KEYBOARD && Fl::event_key() == ' ' &&
       !(Fl::event_state() & (FL_CTRL | FL_SHIFT | FL_ALT | FL_META))) ||
       (event == FL_SHORTCUT && Fl_Widget::test_shortcut()))) {
    take_focus(); track_editor_popup(*this); return 1;
  }
  return Fl_Choice::handle(event);
}
namespace {
// A dismissed popup ends its loop without choosing an item, so no application
// callback runs and the owner keeps its previous state.
bool dismiss_session(Fl_Menu_ &owner) {
  if (!tracking || tracking->tracker.deleted() || &tracking->owner != &owner) return false;
  tracking->finish();
  return true;
}
int open_title(const Fl_Menu_ &owner) {
  if (!tracking || tracking->tracker.deleted() || &tracking->owner != &owner) return -1;
  return tracking->title;
}
} // namespace
namespace {
// Every executable that can open an editor popup links this file, so the
// accessibility layer knows the popups before the first one opens.
[[maybe_unused]] const bool popup_menus_installed = [] {
  a11y::set_popup_menus({&layerc::editor::popup_owner, &layerc::editor::popup_current,
                         &layerc::editor::popup_item_bounds,
                         &layerc::editor::popup_document_window,
                         &layerc::editor::popup_pick, &layerc::editor::track_editor_popup,
                         &layerc::editor::dismiss_session, &layerc::editor::open_title});
  return true;
}();
} // namespace
} // namespace layerc::editor
