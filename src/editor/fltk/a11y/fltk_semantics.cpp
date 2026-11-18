#include "fltk_semantics.hpp"

#include "accessible_widgets.hpp"
#include "editor_scroll.hpp"
#include "fltk_hooks.hpp"

#include "layerc/core/utf8.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace layerc::editor::a11y {

namespace {
std::vector<MenuSize> *pass_sizes{};
std::vector<RowTops> *pass_rows{};
// A query outside a pass (a draw or a list's own scroll check) still needs the
// table, so it is rebuilt per call instead of being kept across passes.
std::vector<RowTops> loose_rows;

Fl_Menu_ *menu_owner(const Target &target) {
  return target.kind == TargetKind::menu_item ? dynamic_cast<Fl_Menu_ *>(target.widget) : nullptr;
}
Fl_Input_ *input_of(const Target &target) {
  return target.kind == TargetKind::widget ? dynamic_cast<Fl_Input_ *>(target.widget) : nullptr;
}
} // namespace

TreePass::TreePass() : previous_(pass_sizes), previous_rows_(pass_rows) {
  pass_sizes = &sizes_;
  pass_rows = &rows_;
}
TreePass::~TreePass() {
  pass_sizes = previous_;
  pass_rows = previous_rows_;
}

const std::vector<int> &row_tops(const RowGeometry &list) {
  std::vector<RowTops> &store = pass_rows ? *pass_rows : loose_rows;
  if (!pass_rows) loose_rows.clear();
  for (const auto &entry : store)
    if (entry.list == &list) return entry.tops;
  store.push_back({&list, list.row_tops()});
  return store.back().tops;
}

std::optional<EditorBounds> list_row_bounds(const RowGeometry &list, int row) {
  const auto *browser = dynamic_cast<const Fl_Browser *>(&list);
  if (!browser || row < 1 || row > browser->size()) return std::nullopt;
  return list.row_bounds(row, row_tops(list));
}

int menu_size(const Fl_Menu_ &menu) {
  if (!pass_sizes) return menu.size();
  for (const auto &entry : *pass_sizes)
    if (entry.menu == &menu && entry.items == menu.menu()) return entry.size;
  pass_sizes->push_back({&menu, menu.menu(), menu.size()});
  return pass_sizes->back().size;
}

const Fl_Menu_Item *menu_item_of(const Target &target) {
  auto *menu = menu_owner(target);
  if (!menu || !menu->menu() || target.index < 0 || target.index >= menu_size(*menu) - 1)
    return nullptr;
  return &menu->menu()[target.index];
}

// Visible items of one inline menu level; FL_SUBMENU_POINTER is not used.
std::vector<int> menu_level(const Fl_Menu_ &menu, int first) {
  std::vector<int> items;
  const auto *array = menu.menu();
  if (!array || first < 0 || first >= menu_size(menu) - 1) return items;
  const Fl_Menu_Item *item = &array[first];
  if (item->label() && !item->visible()) item = item->next();
  for (; item && item->label(); item = item->next())
    items.push_back(static_cast<int>(item - array));
  return items;
}

std::string clean_label(const char *label) {
  std::string result;
  if (label)
    for (const char *p = label; *p; ++p) {
      if (*p == '&' && p[1] != '&') continue;
      result += *p;
      if (*p == '&' && p[1] == '&') ++p;
    }
  return result;
}

std::string accelerator(int shortcut) {
  const int key = shortcut & FL_KEY_MASK;
  if (!shortcut || key < 0x21 || key > 0x7e) return {};
  std::string result;
  if (shortcut & FL_CTRL) result += "<Control>";
  if (shortcut & FL_SHIFT) result += "<Shift>";
  if (shortcut & FL_ALT) result += "<Alt>";
  if (shortcut & FL_META) result += "<Super>";
  return result + static_cast<char>(std::tolower(key));
}

std::string mnemonic(const char *label) {
  if (label)
    for (const char *p = label; *p; ++p) {
      if (*p == '&' && p[1] == '&') { ++p; continue; }
      if (*p == '&' && std::isalnum(static_cast<unsigned char>(p[1])))
        return std::string("<Alt>") +
               static_cast<char>(std::tolower(static_cast<unsigned char>(p[1])));
    }
  return {};
}

std::string valid_utf8(const std::string &text) {
  if (text.find('\0') == std::string::npos && core::is_valid_utf8(text)) return text;
  // Protocols reject invalid UTF-8 and embedded NULs, which file names and
  // pasted selections can contain; replace each offending byte with U+FFFD.
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3
                         : (lead >> 3) == 0x1e ? 4 : 0;
    if (lead != 0 && length != 0 && index + length <= text.size() &&
        core::is_valid_utf8(std::string_view(text).substr(index, length))) {
      result.append(text, index, length);
      index += length;
      continue;
    }
    result += "\xEF\xBF\xBD";
    ++index;
  }
  return result;
}

bool live(const Target &target) {
  // The owning tree keeps the tracker. A newly constructed tracker cannot
  // discover that an arbitrary raw pointer was deleted before this call.
  return target.kind == TargetKind::application || target.widget != nullptr;
}

// Popup menu buttons and unlabeled decoration are not separate semantics.
bool exported(const Fl_Widget &widget) {
  auto *w = const_cast<Fl_Widget *>(&widget);
  if (w == popup_owner()) return true;
  if (w->w() <= 0 || w->h() <= 0) return false;
  if (dynamic_cast<Fl_Box *>(w) && !(w->label() && *w->label())) return false;
  return !(dynamic_cast<Fl_Menu_ *>(w) && !dynamic_cast<Fl_Menu_Bar *>(w) &&
           !dynamic_cast<Fl_Choice *>(w));
}

bool showing(const Target &target) {
  if (!live(target)) return false;
  auto *w = target.widget;
  if (!w) return true;
  if (target.kind == TargetKind::menu_item)
    return popup_item_bounds(*menu_owner(target), target.index).has_value();
  if (target.kind == TargetKind::row)
    if (const auto *list = dynamic_cast<const RowGeometry *>(w)) {
      const auto bounds = list_row_bounds(*list, target.index);
      return bounds && visible_bounds(*w, *bounds).has_value();
    }
  // A page tab is visible whenever its tab strip is, selected or not.
  if (target.kind == TargetKind::widget && dynamic_cast<Fl_Tabs *>(w->parent()))
    return visible_bounds(*w->parent()).has_value();
  return visible_bounds(*w).has_value();
}

bool modal_allows(Fl_Widget &widget) {
  const auto *modal = Fl::modal();
  if (auto *document = popup_document_window(modal)) modal = document;
  return !modal || widget.top_window() == modal;
}

bool enabled(const Target &target) {
  auto *w = target.widget;
  if (!w) return live(target);
  if (!w->active_r() || !modal_allows(*w)) return false;
  const auto *item = menu_item_of(target);
  return target.kind != TargetKind::menu_item || (item && item->active());
}

bool focused(const Target &target) {
  auto *w = target.widget;
  if (!w || !live(target)) return false;
  if (auto *owner = popup_owner())
    return target.kind == TargetKind::menu_item && owner == w &&
           popup_current(*owner, target.index);
  if (target.kind == TargetKind::menu_item || Fl::focus() != w) return false;
  // A focused list reports its current row, as native toolkits do.
  if (target.kind == TargetKind::row) return static_cast<Fl_Browser *>(w)->value() == target.index;
  return dynamic_cast<Fl_Browser *>(w) == nullptr;
}

bool selected(const Target &target) {
  auto *w = target.widget;
  if (!w || !live(target)) return false;
  if (target.kind == TargetKind::row)
    return static_cast<Fl_Browser *>(w)->selected(target.index) != 0;
  if (target.kind == TargetKind::menu_item) {
    if (auto *menu = menu_owner(target); menu && popup_owner() == menu)
      return popup_current(*menu, target.index);
    // Fl_Menu_::value() walks the whole menu; compare the chosen item itself.
    auto *choice = dynamic_cast<Fl_Choice *>(w);
    const auto *item = choice ? menu_item_of(target) : nullptr;
    return item && choice->mvalue() == item;
  }
  if (auto *tabs = dynamic_cast<Fl_Tabs *>(w->parent())) return tabs->value() == w;
  return false;
}

// FLTK's radio behaviour is the button type, not the class: Fl_Radio_Button is a
// plain Fl_Button constructed with FL_RADIO_BUTTON, while Fl_Round_Button's own
// constructor sets FL_TOGGLE_BUTTON and only acts as a radio when retyped.
[[nodiscard]] bool radio_typed(const Fl_Widget *widget) {
  const auto *button = dynamic_cast<const Fl_Button *>(widget);
  return button != nullptr && button->type() == FL_RADIO_BUTTON;
}

bool checked(const Target &target) {
  if (const auto *item = menu_item_of(target))
    return (item->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)) && item->value();
  const Fl_Widget *w = target.kind == TargetKind::widget ? target.widget : nullptr;
  if (const auto *check = dynamic_cast<const Fl_Check_Button *>(w))
    return check->value() != 0;
  if (const auto *round = dynamic_cast<const Fl_Round_Button *>(w)) return round->value() != 0;
  // Any other radio-typed button is a momentary toggle that clears its siblings,
  // so the chosen one is the group's selection and reports itself checked.
  if (const auto *button = dynamic_cast<const Fl_Button *>(w))
    return radio_typed(button) && button->value() != 0;
  return false;
}

Role role_of(const Target &target) {
  auto *w = target.widget;
  if (target.kind == TargetKind::application) return Role::application;
  if (!live(target) || !w) return Role::group;
  if (target.kind == TargetKind::row) return Role::list_row;
  if (target.kind == TargetKind::menu_item) {
    const auto *item = menu_item_of(target);
    if (item && item->submenu()) return Role::menu;
    if (item && (item->flags & FL_MENU_TOGGLE)) return Role::check_menu_item;
    if (item && (item->flags & FL_MENU_RADIO)) return Role::radio_menu_item;
    return Role::menu_item;
  }
  if (const auto registered = accessibility::accessible_role(*w)) return *registered;
  if (auto *window = dynamic_cast<Fl_Window *>(w))
    return window->non_modal() ? Role::dialog : Role::window;
  if (auto *field = dynamic_cast<Fl_Input_ *>(w)) {
    (void)field;
    return Role::text_field;
  }
  if (auto *valuator = dynamic_cast<Fl_Valuator *>(w)) {
    (void)valuator;
    if (dynamic_cast<Fl_Scrollbar *>(w)) return Role::scroll_bar;
    if (dynamic_cast<Fl_Value_Input *>(w)) return Role::number_field;
    if (dynamic_cast<Fl_Slider *>(w)) return Role::slider;
    return Role::number_field;
  }
  if (dynamic_cast<Fl_Menu_Bar *>(w)) return Role::menu_bar;
  if (dynamic_cast<Fl_Choice *>(w)) return Role::drop_down;
  if (dynamic_cast<Fl_Menu_ *>(w)) return Role::menu;
  if (dynamic_cast<Fl_Browser *>(w)) return Role::list;
  if (dynamic_cast<Fl_Tabs *>(w)) return Role::tab_list;
  if (dynamic_cast<Fl_Tabs *>(w->parent())) return Role::tab;
  if (radio_typed(w)) return Role::radio_button;
  if (dynamic_cast<Fl_Check_Button *>(w)) return Role::check_box;
  if (dynamic_cast<Fl_Round_Button *>(w)) return Role::check_box;
  if (dynamic_cast<Fl_Button *>(w)) return Role::button;
  if (dynamic_cast<Fl_Box *>(w)) return Role::label;
  if (dynamic_cast<Fl_Scroll *>(w)) return Role::scroll_pane;
  return Role::group;
}

std::string name(const Target &target) {
  auto *w = target.widget;
  if (!live(target)) return {};
  switch (target.kind) {
  case TargetKind::application: return "LayerE";
  case TargetKind::row: {
    const auto *text = static_cast<Fl_Browser *>(w)->text(target.index);
    return valid_utf8(text ? text : "");
  }
  case TargetKind::menu_item: {
    const auto *item = menu_item_of(target);
    return item ? valid_utf8(clean_label(item->label())) : "";
  }
  case TargetKind::widget: break;
  }
  const auto label = clean_label(w->label());
  // A registered name wins over the label: it is what the control says
  // (DesignSystem.md, Screen readers).
  if (auto registered = accessibility::accessible_name(*w); registered) return valid_utf8(*registered);
  if (!label.empty()) return valid_utf8(label);
  if (dynamic_cast<Fl_Window *>(w)) return "LayerE dialog";
  if (dynamic_cast<Fl_Menu_Bar *>(w)) return "Main menu";
  return valid_utf8(w->tooltip() ? w->tooltip() : "");
}

std::string description(const Target &target, std::string inherited) {
  auto *w = target.widget;
  if (!live(target)) return {};
  std::string text;
  if (w && target.kind == TargetKind::widget) {
    text = accessibility::accessible_description(*w);
    if (text.empty() && !clean_label(w->label()).empty() && w->tooltip()) text = w->tooltip();
  }
  if (text.empty()) text = std::move(inherited);
  text = valid_utf8(text);
  // Nothing exists only as a description: repeating the name says it twice.
  return text == name(target) ? std::string() : text;
}

std::string keybinding(const Target &target) {
  if (const auto *item = menu_item_of(target)) {
    const auto shortcut = accelerator(item->shortcut());
    return shortcut.empty() ? std::string() : ";;" + shortcut;
  }
  auto *button =
      target.kind == TargetKind::widget ? dynamic_cast<Fl_Button *>(target.widget) : nullptr;
  if (!button) return {};
  const auto key = mnemonic(button->label());
  const auto shortcut = accelerator(static_cast<int>(button->shortcut()));
  if (key.empty() && shortcut.empty()) return {};
  return key + ";;" + shortcut;
}

std::vector<std::string> action_names(const Target &target) {
  if (!live(target)) return {};
  if (target.kind == TargetKind::menu_item && !menu_item_of(target)) return {};
  if (target.kind == TargetKind::row || target.kind == TargetKind::menu_item)
    return {"select"};
  switch (role_of(target)) {
  case Role::check_box: return {"toggle"};
  case Role::button: case Role::radio_button: case Role::tab: return {"activate"};
  default: return {};
  }
}

States states(const Target &target) {
  States result;
  auto *w = target.widget;
  if (!live(target)) {
    result.disabled = true;
    result.showing = false;
    result.visible = false;
    return result;
  }
  result.visible = !w || w->visible() != 0;
  result.showing = showing(target);
  const bool is_enabled = enabled(target);
  result.disabled = !is_enabled;
  if (target.kind == TargetKind::widget && w) {
    if (auto *window = dynamic_cast<Fl_Window *>(w)) {
      result.modal = window->modal() != 0;
      result.active = Fl::focus() != nullptr && Fl::focus()->top_window() == window;
      result.resizable = window->resizable() != nullptr;
    } else if (w->visible_focus() && dynamic_cast<Fl_Group *>(w) == nullptr) {
      result.focusable = true;
    } else if (dynamic_cast<Fl_Browser *>(w)) {
      result.focusable = true;
    }
    if (auto *field = dynamic_cast<Fl_Input_ *>(w)) {
      result.read_only = field->readonly() != 0;
      result.editable = field->readonly() == 0;
      result.multi_line = (field->input_type() & FL_MULTILINE_INPUT) != 0;
      result.password = field->input_type() == FL_SECRET_INPUT;
    }
    if (radio_typed(w) || dynamic_cast<Fl_Check_Button *>(w) ||
        dynamic_cast<Fl_Round_Button *>(w))
      result.checkable = true;
    if (dynamic_cast<Fl_Tabs *>(w->parent())) result.selectable = true;
  }
  if (target.kind == TargetKind::row || target.kind == TargetKind::menu_item)
    result.selectable = true;
  if (target.kind == TargetKind::menu_item) result.focusable = true;
  if (const auto *item = menu_item_of(target); item && (item->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)))
    result.checkable = true;
  result.checked = checked(target);
  result.selected = selected(target);
  result.focused = focused(target);
  return result;
}

std::optional<Bounds> local_bounds(const Target &target) {
  auto *w = target.widget;
  if (!live(target) || !w) return std::nullopt;
  auto *window = w->top_window();
  if (!window || !window->shown()) return std::nullopt;
  // A RowGeometry list owns the scroll-aware visible row geometry. Menu items
  // and plain browsers retain their owner bounds; never infer row pitch here.
  int left = w->x(), top = w->y(), width = w->w(), height = w->h();
  if (target.kind == TargetKind::row)
    if (const auto *list = dynamic_cast<const RowGeometry *>(w)) {
      const auto bounds = list_row_bounds(*list, target.index);
      if (!bounds) return std::nullopt;
      left = bounds->x;
      top = bounds->y;
      width = bounds->width;
      height = bounds->height;
    }
  const auto bounds = target.kind == TargetKind::menu_item
                          ? popup_item_bounds(*menu_owner(target), target.index)
                      : w == window ? std::optional<EditorBounds>{{0, 0, w->w(), w->h()}}
                                    : visible_bounds(*w, {left, top, width, height});
  if (!bounds) return std::nullopt;
  return Bounds{static_cast<double>(bounds->x), static_cast<double>(bounds->y),
                static_cast<double>(bounds->width), static_cast<double>(bounds->height)};
}

std::optional<Range> range_of(const Fl_Valuator &valuator) {
  return Range{std::min(valuator.minimum(), valuator.maximum()),
               std::max(valuator.minimum(), valuator.maximum()), valuator.step(),
               valuator.value()};
}

std::string input_text(const Target &target) {
  auto *w = input_of(target);
  return w && w->value() ? valid_utf8(w->value()) : std::string{};
}

bool selection_of(const Fl_Input_ &field, Selection &selection) {
  selection = {};
  if (!field.value() || field.insert_position() == field.mark()) return false;
  selection.begin = static_cast<std::size_t>(std::min(field.insert_position(), field.mark()));
  selection.end = static_cast<std::size_t>(std::max(field.insert_position(), field.mark()));
  return true;
}

bool editable(const Target &target) {
  auto *w = input_of(target);
  // Offset edits on text that is not valid UTF-8 would not match what was exported.
  return w && !w->readonly() && enabled(target) && showing(target) &&
         core::is_valid_utf8(w->value() ? w->value() : "");
}

std::vector<Target> desired_children(const Target &target, const std::vector<Target> &previous) {
  std::vector<Target> children;
  auto *w = target.widget;
  if (!live(target)) return children;
  if (target.kind == TargetKind::application) {
    std::vector<Fl_Window *> shown;
    for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
      if (!window->parent() && !window->menu_window() && !window->tooltip_window())
        shown.push_back(window);
    // Keep known windows in place; FLTK reorders its list on activation.
    for (const auto &child : previous)
      if (auto *window = child.widget;
          window && std::find(shown.begin(), shown.end(), window) != shown.end())
        children.push_back({window, TargetKind::widget, 0});
    for (auto *window : shown)
      if (std::none_of(children.begin(), children.end(),
                       [&](const Target &child) { return child.widget == window; }))
        children.push_back({window, TargetKind::widget, 0});
    return children;
  }
  if (target.kind == TargetKind::menu_item) {
    const auto *item = menu_item_of(target);
    if (item && (item->flags & FL_SUBMENU))
      for (const int index : menu_level(*menu_owner(target), target.index + 1))
        children.push_back({w, TargetKind::menu_item, index});
    return children;
  }
  if (target.kind != TargetKind::widget || !w) return children;
  if (auto *list = dynamic_cast<Fl_Browser *>(w)) {
    // Hidden pages have no reachable rows. Materialize every row when the
    // browser is shown; scrolling must not restrict accessible children.
    if (list->visible_r())
      for (int row = 1; row <= list->size(); ++row)
        children.push_back({w, TargetKind::row, row});
  } else if (auto *menu = dynamic_cast<Fl_Menu_ *>(w)) {
    for (const int index : menu_level(*menu, 0))
      children.push_back({w, TargetKind::menu_item, index});
  } else if (auto *group = w->as_group(); group && dynamic_cast<Fl_Input_ *>(w) == nullptr) {
    for (int index = 0; index < group->children(); ++index)
      if (exported(*group->child(index)))
        children.push_back({group->child(index), TargetKind::widget, 0});
  }
  return children;
}

} // namespace layerc::editor::a11y
