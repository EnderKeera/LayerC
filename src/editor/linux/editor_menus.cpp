#include "editor_menus.hpp"
#include "editor_app.hpp"

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
namespace {
using Command = EditorCommand;
struct Definition {
  const char *path;
  const char *label;
  Command command;
  int shortcut;
  int flags;
};
constexpr std::array definitions{
    Definition{"&File/&New profile", "&New profile", Command::new_profile, FL_CTRL | 'n', 0},
    Definition{"&File/&Import...", "&Import...", Command::import_profiles, FL_CTRL | 'o', 0},
    Definition{"&File/Export &JSON...", "Export &JSON...", Command::export_json, FL_CTRL | FL_SHIFT | 'e', 0},
    Definition{"&File/Export &pack...", "Export &pack...", Command::export_pack, FL_CTRL | FL_ALT | FL_SHIFT | 'e', FL_MENU_DIVIDER},
    Definition{"&File/&Save", "&Save", Command::save, FL_CTRL | 's', 0},
    Definition{"&File/Save &As...", "Save &As...", Command::save_as, FL_CTRL | FL_SHIFT | 's', 0},
    Definition{"&File/Open profiles &folder", "Open profiles &folder", Command::open_folder, 0, FL_MENU_DIVIDER},
    Definition{"&File/&Quit", "&Quit", Command::close, FL_CTRL | 'q', 0},
    Definition{"&Edit/&Undo", "&Undo", Command::undo, FL_CTRL | 'z', 0},
    Definition{"&Edit/&Redo", "&Redo", Command::redo, FL_CTRL | FL_SHIFT | 'z', FL_MENU_DIVIDER},
    Definition{"&Edit/Re&name", "Re&name", Command::rename, FL_F + 2, 0},
    Definition{"&Profile/&Duplicate", "&Duplicate", Command::duplicate, FL_CTRL | 'd', 0},
    Definition{"&Profile/&Delete...", "&Delete...", Command::delete_profile, 0, 0},
    Definition{"&Profile/Toggle saved &enabled", "Toggle saved &enabled", Command::toggle_enabled, 0, 0},
    Definition{"&Cooldowns/&Add cooldown", "&Add cooldown", Command::add_cooldown, 0, 0},
    Definition{"&Cooldowns/D&uplicate cooldown", "D&uplicate cooldown", Command::duplicate_cooldown, 0, 0},
    Definition{"&Cooldowns/&Remove cooldown...", "&Remove cooldown...", Command::delete_cooldown, 0, 0},
    Definition{"&Bindings/Add &key", "Add &key", Command::add_keyboard, 0, 0},
    Definition{"&Bindings/Add &mouse", "Add &mouse", Command::add_mouse, 0, 0},
    Definition{"&Bindings/&Capture focused chord...", "&Capture focused chord...", Command::capture_binding, 0, 0},
    Definition{"&Bindings/&Remove binding", "&Remove binding", Command::delete_binding, 0, 0},
    Definition{"&Panels/&Add panel", "&Add panel", Command::add_panel, 0, 0},
    Definition{"&Panels/&Remove panel...", "&Remove panel...", Command::delete_panel, 0, 0},
    Definition{"&Panels/Choose &colours...", "Choose &colours...", Command::panel_colors, 0, 0},
    Definition{"&Panels/&Position on window", "&Position on window", Command::position, 0, 0},
    Definition{"&View/&Profile", "&Profile", Command::profile_page, FL_CTRL | '1', 0},
    Definition{"&View/&Cooldowns", "&Cooldowns", Command::cooldown_page, FL_CTRL | '2', 0},
    Definition{"&View/&Bindings", "&Bindings", Command::binding_page, FL_CTRL | '3', 0},
    Definition{"&View/&Panels", "&Panels", Command::panel_page, FL_CTRL | '4', FL_MENU_DIVIDER},
    Definition{"&View/&Automation", "&Automation", Command::automation_page, FL_CTRL | '5', 0},
    Definition{"&View/&Visual", "&Visual", Command::visual_page, FL_CTRL | '6', 0},
    Definition{"&View/Daemon &status", "Daemon &status", Command::refresh, 0, 0},
    Definition{"&View/Runtime &diagnostics...", "Runtime &diagnostics...", Command::diagnostics, 0, 0},
    Definition{"&Tools/&Options...", "&Options...", Command::options, FL_CTRL | ',', 0},
    Definition{"&Tools/A&ppearance...", "A&ppearance...", Command::appearance, 0, FL_MENU_DIVIDER},
    Definition{"&Tools/Split grouped &bars", "Split grouped &bars", Command::split_bars, 0, FL_MENU_DIVIDER},
    Definition{"&Tools/Detected &tasks...", "Detected &tasks...", Command::tasks, FL_CTRL | 't', 0},
    Definition{"&Help/&Keyboard shortcuts", "&Keyboard shortcuts", Command::shortcuts_help, FL_F + 1, 0},
    Definition{"&Help/&About LayerE", "&About LayerE", Command::about, 0, 0}};
static_assert(static_cast<std::size_t>(Command::close) < 48);

bool in_context(Command command, EditorMenuContext context) {
  switch (context) {
  case EditorMenuContext::profiles:
    return command == Command::new_profile || command == Command::import_profiles ||
           command == Command::duplicate || command == Command::delete_profile ||
           command == Command::toggle_enabled || command == Command::save ||
           command == Command::export_json || command == Command::export_pack;
  case EditorMenuContext::cooldowns:
    return command == Command::add_cooldown || command == Command::duplicate_cooldown ||
           command == Command::delete_cooldown;
  case EditorMenuContext::panels:
    return command == Command::add_panel || command == Command::delete_panel ||
           command == Command::panel_colors;
  case EditorMenuContext::bindings:
    return command == Command::add_keyboard || command == Command::add_mouse ||
           command == Command::capture_binding || command == Command::delete_binding;
  }
  return false;
}
bool needs_target(Command command, EditorMenuContext context) {
  if (context == EditorMenuContext::profiles)
    return command != Command::new_profile && command != Command::import_profiles;
  return command == Command::delete_cooldown || command == Command::duplicate_cooldown ||
         command == Command::delete_panel || command == Command::panel_colors ||
         command == Command::delete_binding;
}
} // namespace

EditorMenus::EditorMenus(EditorApp &app) : app_(app) {
  bar_ = new Fl_Menu_Bar(0, 0, app.window.w(), bar_height);
  for (std::size_t i = 0; i < actions_.size(); ++i)
    actions_[i] = Action{&app, static_cast<Command>(i)};
  for (const auto &item : definitions)
    bar_->add(item.path, item.shortcut, invoke,
              &actions_[static_cast<std::size_t>(item.command)], item.flags);
}
void EditorMenus::invoke(Fl_Widget *widget, void *data) {
  auto &action = *static_cast<Action *>(data);
  if (auto *menu = dynamic_cast<Fl_Menu_ *>(widget)) {
    // A stale accessibility/test callback must obey this menu's disabled
    // entry, not just the global document state.
    for (int i = 0; i < menu->size(); ++i)
      if (menu->menu()[i].user_data() == data && !menu->menu()[i].active()) return;
  }
  if (action.app->command_enabled(action.command)) action.app->command(action.command);
}
std::string EditorMenus::shortcut_help() {
  // Derived from the menu definitions, so the help cannot drift from them.
  std::string text;
  for (const auto &item : definitions) {
    if (!item.shortcut) continue;
    std::string label = item.label;
    std::erase(label, '&');
    text += std::string(fl_shortcut_label(static_cast<unsigned>(item.shortcut))) + "\t" + label + "\n";
  }
  return text + "Ctrl+Y\tRedo\n"
                "Tab / Shift+Tab\tMove between fields\n"
                "Alt+letter\tOpen the menu with that underlined letter\n"
                "Right-click\tList context menu\n"
                "Arrow keys\tChange the focused list, choice or tab";
}
void EditorMenus::update() {
  for (const auto &item : definitions) {
    const int index = bar_->find_index(item.path);
    if (index < 0) continue;
    bar_->mode(index, item.flags |
        (app_.command_enabled(item.command) ? 0 : FL_MENU_INACTIVE));
  }
  bar_->redraw();
}
bool EditorMenus::shortcut(int key, int modifiers) {
  // Keep Ctrl+A/C/V/X and other native text-editing keys untouched.
  constexpr int mask = FL_CTRL | FL_SHIFT | FL_ALT | FL_META;
  if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
  // Ctrl+Y is the Windows editor's redo; Ctrl+Shift+Z stays the menu label.
  if (key == 'y' && (modifiers & mask) == FL_CTRL) {
    if (app_.command_enabled(EditorCommand::redo)) app_.command(EditorCommand::redo);
    return true;
  }
  for (const auto &item : definitions) {
    if (!item.shortcut || key != (item.shortcut & FL_KEY_MASK) ||
        (modifiers & mask) != (item.shortcut & mask)) continue;
    if (app_.command_enabled(item.command)) app_.command(item.command);
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
  return app_.menus && (!Fl::modal() || Fl::modal() == this) &&
         (!Fl::grab() || Fl::grab() == this) &&
         app_.menus->shortcut(Fl::event_key(), Fl::event_state());
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
  for (const auto &item : definitions) {
    if (!in_context(item.command, context)) continue;
    const bool available = app_.command_enabled(item.command) &&
                           (has_target || !needs_target(item.command, context));
    menu.add(item.label, 0, invoke, &actions_[static_cast<std::size_t>(item.command)],
             available ? 0 : FL_MENU_INACTIVE);
  }
}
void EditorMenus::popup(EditorMenuContext context, bool has_target) {
  // The temporary menu is a child of our window, never another application.
  auto *previous = Fl_Group::current();
  Fl_Group::current(&app_.window);
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
  app_.menus->popup(context_, row > 0);
  return 1;
}
} // namespace layerc::editor
