#include "editor_menus.hpp"
#include "editor_app.hpp"
#include "platform/editor_options.hpp"
#include "editor_style.hpp"
#include "editor_popup.hpp"

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

constexpr const char *text_size_paths[]{
    "&View/&Text size/&100 %", "&View/&Text size/11&5 %", "&View/&Text size/1&30 %"};
template <std::size_t index> void choose_text_size(Fl_Widget *, void *) {
  select_editor_text_scale(text_scale_choices[index]);
}
constexpr Fl_Callback *text_size_callbacks[]{
    choose_text_size<0>, choose_text_size<1>, choose_text_size<2>};

} // namespace

EditorMenus::EditorMenus(EditorApp &app, Fl_Window &window)
    : app_(app), window_(window) {
  bar_ = new EditorMenuBar(0, 0, window_.w(), bar_height);
  // A strip with a bottom divider; its drop-down menus keep a full outline.
  bar_->box(editor_menu_bar_box());
  bar_->menu_box(FL_UP_BOX);
  for (std::size_t i = 0; i < actions_.size(); ++i)
    actions_[i] = Action{&app, static_cast<EditorCommand>(i)};
  for (const auto &spec : editor_command_specs())
    if (spec.menu_path != nullptr)
      bar_->add(spec.menu_path, spec.shortcut, invoke,
                &actions_[static_cast<std::size_t>(spec.command)], spec.menu_flags);
  if constexpr (windows_editor_keys)
    for (std::size_t i = 0; i < text_scale_choices.size(); ++i)
      bar_->add(text_size_paths[i], 0, text_size_callbacks[i], nullptr, FL_MENU_RADIO);
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
  if constexpr (windows_editor_keys)
    for (std::size_t i = 0; i < text_scale_choices.size(); ++i) {
      const auto index = bar_->find_index(text_size_paths[i]);
      if (index >= 0)
        bar_->mode(index, FL_MENU_RADIO |
            (text_scale_choices[i] == current_editor_text_scale() ? FL_MENU_VALUE : 0));
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
  if (event == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
    // Esc in any editor window, popup or dialog ends Direct drag before the
    // focused popup or dialog takes it; their own Esc still runs afterwards.
    bool ended = false;
    for (auto *const owner : std::vector<EditorWindow *>(owners))
      ended = owner->app_.cancel_direct_drag() || ended;
    // The main window would close on Esc, so there the key ends only the mode.
    if (ended && !Fl::grab() && !Fl::modal() &&
        std::find(owners.begin(), owners.end(), window) != owners.end())
      return 1;
  }
  if (event == FL_KEYBOARD && history_key()) {
    const auto owner = std::find(owners.begin(), owners.end(), window);
    if (owner != owners.end() && (*owner)->document_shortcut()) return 1;
  }
  return Fl::handle_(event, window);
}
int EditorWindow::handle(int event) {
  // Esc in the editor ends Direct drag before FLTK's Esc-closes-window path.
  if (event == FL_SHORTCUT && Fl::event_key() == FL_Escape && app_.cancel_direct_drag())
    return 1;
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
    const int shortcut = windows_editor_keys && spec.command == EditorCommand::duplicate_cooldown
        ? spec.shortcut : 0;
    menu.add(spec.label, shortcut, invoke, &actions_[static_cast<std::size_t>(spec.command)],
             available ? 0 : FL_MENU_INACTIVE);
  }
}
void EditorMenus::popup(EditorMenuContext context, bool has_target,
                        const std::optional<EditorBounds> anchor) {
  // The temporary menu is a child of our window, never another application.
  auto *previous = Fl_Group::current();
  Fl_Group::current(&window_);
  Fl_Menu_Button menu(Fl::event_x(), Fl::event_y(), 0, 0);
  Fl_Group::current(previous);
  configure_context(menu, context, has_target);
  track_editor_popup(menu, -1, anchor);
}

EditorEmptyState editor_empty_state(EditorMenuContext context) noexcept {
  switch (context) {
  case EditorMenuContext::profiles: return {"No profiles", "Press New to create a profile."};
  case EditorMenuContext::cooldowns: return {"No cooldowns", "Press Add cooldown to create one."};
  case EditorMenuContext::bindings: return {"No bindings", "Press Add key or Add mouse to add one."};
  case EditorMenuContext::panels: return {"No panels", "Press Add panel to create one."};
  }
  return {"No items", "Choose an add action to begin."};
}

namespace {
constexpr std::array empty_list_text{
    PaintedText{FixedInk::secondary, PaintedGround::own_fill, PaintedPurpose::empty_list_title, true},
    PaintedText{FixedInk::subtext, PaintedGround::own_fill, PaintedPurpose::empty_list_hint, true}};
} // namespace

std::span<const PaintedText> ContextBrowser::painted_text() const noexcept { return empty_list_text; }

void draw_editor_empty_state(const EditorBounds &bounds, EditorEmptyState copy,
                             Fl_Font face, Fl_Fontsize size) {
  if (bounds.width <= 16 || bounds.height <= 0) return;
  const auto old_color = fl_color();
  const auto old_face = fl_font();
  const auto old_size = fl_size();
  const auto ink = [](std::uint32_t rgb) {
    return fl_rgb_color(static_cast<uchar>(rgb >> 16U),
                        static_cast<uchar>(rgb >> 8U),
                        static_cast<uchar>(rgb));
  };
  fl_font(face, size);
  const int row = fl_height() + 4;
  const int top = bounds.y + std::max(0, (bounds.height - row * 3) / 2);
  fl_push_clip(bounds.x, bounds.y, bounds.width, bounds.height);
  const auto tokens = current_editor_tokens();
  fl_color(ink(fixed_ink(tokens, empty_list_text[0].ink)));
  fl_draw(copy.title, bounds.x + 8, top, bounds.width - 16, row,
          FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  fl_color(ink(fixed_ink(tokens, empty_list_text[1].ink)));
  fl_draw(copy.hint, bounds.x + 8, top + row, bounds.width - 16, row * 2,
          FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  fl_pop_clip();
  fl_font(old_face, old_size);
  fl_color(old_color);
}

ContextBrowser::ContextBrowser(int x, int y, int width, int height, const char *label,
                               EditorApp &app, EditorMenuContext context)
    : Fl_Hold_Browser(x, y, width, height, label), app_(app), context_(context) { tooltip(editor_empty_state(context_).hint); }
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
std::vector<int> ContextBrowser::row_tops() const {
  std::vector<int> tops;
  tops.reserve(static_cast<std::size_t>(std::max(0, size())) + 1);
  // Rows start where FLTK draws them: the viewport top inside the box, the
  // origin row_at() hit-tests against.
  int left{}, top{}, width{}, height{};
  bbox(left, top, width, height);
  int row_top = top - vposition();
  for (auto *item = item_first(); item; item = item_next(item)) {
    tops.push_back(row_top);
    row_top += item_height(item);
  }
  tops.push_back(row_top);
  return tops;
}
std::optional<EditorBounds> ContextBrowser::row_bounds(int row) {
  const std::vector<int> tops = row_tops();
  return row_bounds(row, tops);
}
std::optional<EditorBounds> ContextBrowser::row_bounds(int row,
                                                       const std::vector<int> &tops) const {
  if (!visible_r() || row < 1 || row > size()) return std::nullopt;
  int left{}, top{}, width{}, height{};
  bbox(left, top, width, height);
  if (width <= 0 || height <= 0) return std::nullopt;
  const auto index = static_cast<std::size_t>(row - 1);
  if (tops.size() <= index + 1) return std::nullopt;
  const int visible_top = std::max(top, tops[index]);
  const int bottom = std::min(top + height, tops[index + 1]);
  if (bottom <= visible_top) return std::nullopt;
  return EditorBounds{left, visible_top, width, bottom - visible_top};
}
void ContextBrowser::draw() {
  const bool show_focus = visible_focus() != 0;
  clear_visible_focus();
  Fl_Hold_Browser::draw();
  if (show_focus) set_visible_focus();
  if (size() == 0) {
    int left{}, top{}, width{}, height{};
    bbox(left, top, width, height);
    draw_editor_empty_state({left, top, width, height}, editor_empty_state(context_),
                            textfont(), textsize());
  }
  const bool focused = Fl::focus() == this && active_r() && show_focus && Fl::visible_focus();
  const auto accent = current_editor_tokens().accent;
  const auto color = fl_rgb_color(static_cast<uchar>(accent >> 16U),
                                 static_cast<uchar>(accent >> 8U), static_cast<uchar>(accent));
  if (focused) {
    fl_color(color);
    fl_rectf(x(), y() + std::max(0, h() - 2), w(), std::min(h(), 2));
  }
  if (const auto row = row_bounds(value())) {
    fl_push_clip(row->x, row->y, row->width, row->height);
    if (focused) draw_editor_focus(row->x, row->y, row->width, row->height);
    else {
      fl_color(color);
      fl_rectf(row->x, row->y, std::min(3, row->width), row->height);
    }
    fl_pop_clip();
  }
}

int ContextBrowser::handle(int event) {
  if (event == FL_FOCUS || event == FL_UNFOCUS) {
    const int result = Fl_Hold_Browser::handle(event);
    redraw();
    return result;
  }
  if constexpr (windows_editor_keys) {
    if (event == FL_KEYDOWN && Fl::focus() == this && active_r()) {
      constexpr int modifiers = FL_CTRL | FL_SHIFT | FL_ALT | FL_META;
      const int shortcut = Fl::event_key() | (Fl::event_state() & modifiers);
      for (const auto &item : editor_list_shortcuts()) {
        if (item.shortcut != shortcut || (item.contexts & context_bit(context_)) == 0)
          continue;
        const int row = value();
        const bool has_target = row > 0 && row <= size();
        if (item.command) {
          if (has_target) run_command(app_, *item.command);
        } else {
          if (has_target && !row_bounds(row)) display(row);
          app_.show_context_menu(context_, has_target,
              row_bounds(row).value_or(EditorBounds{x(), y(), 0, 0}));
        }
        return 1;
      }
    }
  }
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
