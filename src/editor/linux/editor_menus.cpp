#include "editor_menus.hpp"
#include "editor_app.hpp"
#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace layerc::editor {

EditorMenus::EditorMenus(EditorApp &app, Fl_Window &window)
    : app_(app), window_(window) {
  bar_ = new Fl_Menu_Bar(0, 0, window_.w(), bar_height);
  // A strip with a bottom divider; its drop-down menus keep a full outline.
  bar_->box(editor_menu_bar_box());
  bar_->menu_box(FL_UP_BOX);
  for (std::size_t i = 0; i < actions_.size(); ++i)
    actions_[i] = Action{&app, static_cast<EditorCommand>(i)};
  for (const auto &spec : editor_command_specs())
    if (spec.menu_path != nullptr)
      bar_->add(spec.menu_path, spec.shortcut, invoke,
                &actions_[static_cast<std::size_t>(spec.command)], spec.menu_flags);
}
void EditorMenus::invoke(Fl_Widget *widget, void *data) {
  auto &action = *static_cast<Action *>(data);
  if (auto *menu = dynamic_cast<Fl_Menu_ *>(widget)) {
    // A stale accessibility/test callback must obey this menu's disabled
    // entry, not just the global document state.
    for (int i = 0; i < menu->size(); ++i)
      if (menu->menu()[i].user_data() == data && !menu->menu()[i].active()) return;
  }
  run_command(*action.app, action.command);
}
std::string EditorMenus::shortcut_help() { return layerc::editor::shortcut_help(); }
void EditorMenus::update() {
  for (const auto &spec : editor_command_specs()) {
    if (spec.menu_path == nullptr) continue;
    const int index = bar_->find_index(spec.menu_path);
    if (index < 0) continue;
    bar_->mode(index, spec.menu_flags |
        (command_enabled(app_, spec.command) ? 0 : FL_MENU_INACTIVE));
  }
  bar_->redraw();
}
bool EditorMenus::shortcut(int key, int modifiers) {
  // Keep Ctrl+A/C/V/X and other native text-editing keys untouched.
  constexpr int mask = FL_CTRL | FL_SHIFT | FL_ALT | FL_META;
  if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
  for (const auto &spec : editor_command_specs())
    for (const int shortcut : {spec.shortcut, spec.alternate_shortcut}) {
      if (shortcut == 0 || key != (shortcut & FL_KEY_MASK) ||
          (modifiers & mask) != (shortcut & mask))
        continue;
      run_command(app_, spec.command);
      // A disabled document shortcut must not fall through to local text undo.
      return true;
    }
  return false;
}

namespace {
// Live editor windows; each pre-empts history keys sent to itself.
std::vector<EditorWindow *> &keyboard_owners() {
  static std::vector<EditorWindow *> owners;
  return owners;
}
// Only document history keys pre-empt a focused field; every other shortcut
// is offered to the field first, then reaches the window as FL_SHORTCUT.
bool history_key() {
  const int key = Fl::event_key();
  return (Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) == FL_CTRL &&
         (key == 'z' || key == 'y');
}
}
EditorWindow::EditorWindow(EditorApp &app, int width, int height, const char *title)
    : Fl_Double_Window(width, height, title), app_(app) {
  keyboard_owners().push_back(this);
  Fl::event_dispatch(dispatch);
}
EditorWindow::~EditorWindow() {
  auto &owners = keyboard_owners();
  owners.erase(std::remove(owners.begin(), owners.end(), this), owners.end());
  if (owners.empty()) Fl::event_dispatch(nullptr);
}
bool EditorWindow::document_shortcut() {
  return (!Fl::modal() || Fl::modal() == this) &&
         (!Fl::grab() || Fl::grab() == this) &&
         app_.handle_shortcut(Fl::event_key(), Fl::event_state());
}
// FLTK offers a key to the focused widget before its window, so a focused
// input would consume Ctrl+Z as local undo. Match document history first;
// popup menus, dialogs and other windows keep their own keyboard handling.
int EditorWindow::dispatch(int event, Fl_Window *window) {
  const auto &owners = keyboard_owners();
  if (event == FL_KEYBOARD && history_key()) {
    const auto owner = std::find(owners.begin(), owners.end(), window);
    if (owner != owners.end() && (*owner)->document_shortcut()) return 1;
  }
  return Fl::handle_(event, window);
}
int EditorWindow::handle(int event) {
  if (event == FL_SHORTCUT && document_shortcut()) return 1;
  return Fl_Double_Window::handle(event);
}

void EditorMenus::configure_context(Fl_Menu_Button &menu, EditorMenuContext context,
                                    bool has_target) {
  menu.clear();
  const unsigned bit = context_bit(context);
  for (const auto &spec : editor_command_specs()) {
    if ((spec.contexts & bit) == 0) continue;
    const bool available = command_enabled(app_, spec.command) &&
                           (has_target || (spec.target_contexts & bit) == 0);
    menu.add(spec.label, 0, invoke, &actions_[static_cast<std::size_t>(spec.command)],
             available ? 0 : FL_MENU_INACTIVE);
  }
}
void EditorMenus::popup(EditorMenuContext context, bool has_target) {
  // The temporary menu is a child of our window, never another application.
  auto *previous = Fl_Group::current();
  Fl_Group::current(&window_);
  Fl_Menu_Button menu(Fl::event_x(), Fl::event_y(), 0, 0);
  Fl_Group::current(previous);
  configure_context(menu, context, has_target);
  (void)menu.popup();
}

ContextBrowser::ContextBrowser(int x, int y, int width, int height, const char *label,
                               EditorApp &app, EditorMenuContext context)
    : Fl_Hold_Browser(x, y, width, height, label), app_(app), context_(context) {}
int ContextBrowser::row_at(int x, int y) {
  int left{}, top{}, width{}, height{};
  bbox(left, top, width, height);
  if (x < left || y < top || x >= left + width || y >= top + height) return 0;
  int row_top = top - vposition();
  int row = 1;
  for (auto *item = item_first(); item; item = item_next(item), ++row) {
    const int row_height = item_height(item);
    if (y >= row_top && y < row_top + row_height) return row;
    row_top += row_height;
  }
  return 0;
}
std::optional<EditorBounds> ContextBrowser::row_bounds(int row) {
  if (!visible_r() || row < 1 || row > size()) return std::nullopt;
  int left{}, top{}, width{}, height{};
  bbox(left, top, width, height);
  if (width <= 0 || height <= 0) return std::nullopt;
  int row_top = top - vposition();
  int index = 1;
  for (auto *item = item_first(); item; item = item_next(item), ++index) {
    const int row_height = item_height(item);
    if (index == row) {
      const int visible_top = std::max(top, row_top);
      const int bottom = std::min(top + height, row_top + row_height);
      if (bottom <= visible_top) return std::nullopt;
      return EditorBounds{left, visible_top, width, bottom - visible_top};
    }
    row_top += row_height;
  }
  return std::nullopt;
}
int ContextBrowser::handle(int event) {
  if (event != FL_PUSH || Fl::event_button() != FL_RIGHT_MOUSE)
    return Fl_Hold_Browser::handle(event);
  const int row = row_at(Fl::event_x(), Fl::event_y());
  if (row > 0) {
    if (context_ == EditorMenuContext::profiles) {
      const auto index = static_cast<std::size_t>(row - 1);
      if (index >= app_.session.profiles().profiles.size()) return 1;
      // A dirty-save prompt may resort the profile list. Pin the pathname,
      // rather than treating the old numeric row as authority afterward.
      const auto path = app_.session.profiles().profiles[index].path;
      value(row);
      app_.selection_changed(this);
      if (!app_.session.has_document() || app_.session.document().path() != path) return 1;
    } else {
      value(row);
      app_.selection_changed(this);
      if (value() != row) return 1; // Rejected navigation kept the old selection.
    }
  }
  app_.show_context_menu(context_, row > 0);
  return 1;
}
} // namespace layerc::editor
