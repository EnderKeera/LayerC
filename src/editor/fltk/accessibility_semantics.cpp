#include "accessibility_semantics.hpp"
#include "accessible_widgets.hpp"
#include "editor_app.hpp"
#include "editor_menus.hpp"
#include "calibration_window.hpp"
#include "editor_scroll.hpp"
#include "editor_popup.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Valuator.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor::accessibility {

namespace {
std::vector<MenuSize> *pass_sizes{};
}

struct LayerCSemantic { AtkObject parent; Node *node; };
struct LayerCSemanticClass { AtkObjectClass parent; };
struct LayerCTextSemantic { LayerCSemantic parent; };
struct LayerCTextSemanticClass { LayerCSemanticClass parent; };
struct LayerCActionSemantic { LayerCSemantic parent; };
struct LayerCActionSemanticClass { LayerCSemanticClass parent; };
struct LayerCValueSemantic { LayerCSemantic parent; };
struct LayerCValueSemanticClass { LayerCSemanticClass parent; };
struct LayerCSelectionSemantic { LayerCSemantic parent; };
struct LayerCSelectionSemanticClass { LayerCSemanticClass parent; };
struct LayerCWindowSemantic { LayerCSemantic parent; };
struct LayerCWindowSemanticClass { LayerCSemanticClass parent; };

void component_init(AtkComponentIface *);
void text_init(AtkTextIface *);
void editable_init(AtkEditableTextIface *);
void action_init(AtkActionIface *);
void value_init(AtkValueIface *);
void selection_init(AtkSelectionIface *);
void window_init(AtkWindowIface *) {}
G_DEFINE_TYPE_WITH_CODE(LayerCSemantic, semantic, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, component_init))
G_DEFINE_TYPE_WITH_CODE(LayerCTextSemantic, text_semantic, semantic_get_type(),
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, text_init)
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_EDITABLE_TEXT, editable_init))
G_DEFINE_TYPE_WITH_CODE(LayerCActionSemantic, action_semantic, semantic_get_type(),
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, action_init))
G_DEFINE_TYPE_WITH_CODE(LayerCValueSemantic, value_semantic, semantic_get_type(),
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_VALUE, value_init))
G_DEFINE_TYPE_WITH_CODE(LayerCSelectionSemantic, selection_semantic, semantic_get_type(),
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_SELECTION, selection_init))
G_DEFINE_TYPE_WITH_CODE(LayerCWindowSemantic, window_semantic, semantic_get_type(),
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_WINDOW, window_init))

Node &node(void *object) { return *reinterpret_cast<LayerCSemantic *>(object)->node; }
bool live(const Node &n) {
  return !n.retired && (n.kind == Kind::application || n.widget() != nullptr);
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
// GTK accelerator notation, as AT-SPI clients expect, for printable keys.
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
        return std::string("<Alt>") + static_cast<char>(std::tolower(static_cast<unsigned char>(p[1])));
    }
  return {};
}

TreePass::TreePass() : previous_(pass_sizes) { pass_sizes = &sizes_; }
TreePass::~TreePass() { pass_sizes = previous_; }
int menu_size(const Fl_Menu_ &menu) {
  if (!pass_sizes) return menu.size();
  for (const auto &entry : *pass_sizes)
    if (entry.menu == &menu && entry.items == menu.menu()) return entry.size;
  pass_sizes->push_back({&menu, menu.menu(), menu.size()});
  return pass_sizes->back().size;
}

Fl_Menu_ *menu_owner(const Node &n) { return dynamic_cast<Fl_Menu_ *>(n.widget()); }
const Fl_Menu_Item *menu_item(const Node &n) {
  auto *menu = menu_owner(n);
  if (n.kind != Kind::menu_item || !menu || !menu->menu() || n.index < 0 ||
      n.index >= menu_size(*menu) - 1)
    return nullptr;
  return &menu->menu()[n.index];
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

bool selected(const Node &n) {
  auto *w = n.widget();
  if (!w || !live(n)) return false;
  if (n.kind == Kind::row) return static_cast<Fl_Browser *>(w)->selected(n.index) != 0;
  if (n.kind == Kind::menu_item) {
    if (auto *menu = menu_owner(n); menu && popup_owner() == menu)
      return popup_current(*menu, n.index);
    // Fl_Menu_::value() walks the whole menu; compare the chosen item itself.
    auto *choice = dynamic_cast<Fl_Choice *>(w);
    const auto *item = choice ? menu_item(n) : nullptr;
    return item && choice->mvalue() == item;
  }
  if (auto *tabs = dynamic_cast<Fl_Tabs *>(w->parent())) return tabs->value() == w;
  return false;
}
bool focused(const Node &n) {
  auto *w = n.widget();
  if (!w || !live(n)) return false;
  if (auto *owner = popup_owner())
    return n.kind == Kind::menu_item && owner == w && popup_current(*owner, n.index);
  if (n.kind == Kind::menu_item || Fl::focus() != w) return false;
  // A focused list reports its current row, as native toolkits do.
  if (n.kind == Kind::row) return static_cast<Fl_Browser *>(w)->value() == n.index;
  return dynamic_cast<Fl_Browser *>(w) == nullptr;
}
bool showing(const Node &n) {
  if (!live(n)) return false;
  auto *w = n.widget();
  if (!w) return true;
  if (n.kind == Kind::menu_item)
    return popup_item_bounds(*menu_owner(n), n.index).has_value();
  if (n.kind == Kind::row)
    if (auto *list = dynamic_cast<ContextBrowser *>(w)) {
      const auto bounds = list->row_bounds(n.index);
      return bounds && visible_bounds(*w, *bounds).has_value();
    }
  // A page tab is visible whenever its tab strip is, selected or not.
  if (n.kind == Kind::widget && dynamic_cast<Fl_Tabs *>(w->parent()))
    return visible_bounds(*w->parent()).has_value();
  return visible_bounds(*w).has_value();
}
bool modal_allows(Fl_Widget &widget) {
  const auto *modal = Fl::modal();
  if (auto *document = popup_document_window(modal)) modal = document;
  return !modal || widget.top_window() == modal;
}
bool enabled(const Node &n) {
  auto *w = n.widget();
  if (!w) return live(n);
  if (!w->active_r() || !modal_allows(*w)) return false;
  const auto *item = menu_item(n);
  return n.kind != Kind::menu_item || (item && item->active());
}
bool checked(const Node &n) {
  if (const auto *item = menu_item(n))
    return (item->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)) && item->value();
  auto *check = n.kind == Kind::widget ? dynamic_cast<Fl_Check_Button *>(n.widget()) : nullptr;
  return check && check->value();
}
Fl_Input_ *input(void *object) {
  auto &n = node(object);
  return n.kind == Kind::widget && live(n) ? dynamic_cast<Fl_Input_ *>(n.widget()) : nullptr;
}
// D-Bus rejects (by default, aborts on) invalid UTF-8, which file names and
// pasted X selections can contain; every exported string passes through here.
std::string valid_utf8(const std::string &text) {
  if (g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return text;
  gchar *fixed = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
  std::string result = fixed;
  g_free(fixed);
  return result;
}
std::string input_text(void *object) {
  auto *w = input(object);
  return w && w->value() ? valid_utf8(w->value()) : std::string{};
}

std::string raw_name(const Node &n) {
  auto *w = n.widget();
  if (!live(n)) return {};
  switch (n.kind) {
  case Kind::application: return "LayerE";
  case Kind::row: {
    const auto *text = static_cast<Fl_Browser *>(w)->text(n.index);
    return text ? text : "";
  }
  case Kind::menu_item: {
    const auto *item = menu_item(n);
    return item ? clean_label(item->label()) : "";
  }
  case Kind::widget: break;
  }
  auto name = clean_label(w->label());
  // A registered name wins over the label: it is what the control says
  // (DesignSystem.md, Screen readers).
  if (auto registered = accessible_name(*w); registered) return *registered;
  if (!name.empty()) return name;
  if (dynamic_cast<Fl_Window *>(w)) return "LayerE dialog";
  if (dynamic_cast<PanelPreview *>(w)) return "Panel placement preview";
  if (dynamic_cast<Fl_Menu_Bar *>(w)) return "Main menu";
  return w->tooltip() ? w->tooltip() : "";
}
std::string current_keybinding(const Node &n) {
  if (const auto *item = menu_item(n)) {
    const auto shortcut = accelerator(item->shortcut());
    return shortcut.empty() ? std::string() : ";;" + shortcut;
  }
  auto *button = n.kind == Kind::widget ? dynamic_cast<Fl_Button *>(n.widget()) : nullptr;
  if (!button) return {};
  const auto key = mnemonic(button->label());
  const auto shortcut = accelerator(static_cast<int>(button->shortcut()));
  if (key.empty() && shortcut.empty()) return {};
  return key + ";;" + shortcut;
}

std::string current_name(const Node &n) { return valid_utf8(raw_name(n)); }
// The line that belongs to a control is its description: the registry's, else
// the tooltip a labelled control does not already say as its name, else what
// the adaptor itself stored, such as the panel preview's static text.
std::string current_description(AtkObject *object) {
  auto &n = node(object);
  if (!live(n)) return {};
  std::string description;
  if (n.kind == Kind::widget)
    if (auto *w = n.widget()) {
      description = accessible_description(*w);
      if (description.empty() && !clean_label(w->label()).empty() && w->tooltip())
        description = w->tooltip();
    }
  if (description.empty()) {
    const auto inherited = ATK_OBJECT_CLASS(semantic_parent_class)->get_description;
    if (inherited != nullptr)
      if (const gchar *text = inherited(object); text != nullptr) description = text;
  }
  description = valid_utf8(description);
  // Nothing exists only as a description: repeating the name says it twice.
  return description == current_name(n) ? std::string() : description;
}
// Answers from its own buffer: overwriting the last reported name here would
// hide the change from report_changes and leave other clients' caches stale.
const gchar *get_name(AtkObject *object) {
  auto &n = node(object);
  n.name_reply = current_name(n);
  return n.name_reply.c_str();
}
const gchar *get_description(AtkObject *object) {
  auto &n = node(object);
  n.description_reply = current_description(object);
  return n.description_reply.c_str();
}
AtkObject *get_parent(AtkObject *object) { return node(object).parent; }
gint get_n_children(AtkObject *object) {
  auto &n = node(object);
  return live(n) ? static_cast<gint>(n.children.size()) : 0;
}
AtkObject *ref_child(AtkObject *object, gint index) {
  auto &n = node(object);
  if (!live(n) || index < 0 || static_cast<std::size_t>(index) >= n.children.size()) return nullptr;
  return ATK_OBJECT(g_object_ref(n.children[static_cast<std::size_t>(index)]));
}
gint index_in_parent(AtkObject *object) {
  auto *parent = node(object).parent;
  if (!parent) return -1;
  const auto &children = node(parent).children;
  const auto found = std::find(children.begin(), children.end(), object);
  return found == children.end() ? -1 : static_cast<gint>(found - children.begin());
}
AtkStateSet *ref_states(AtkObject *object) {
  auto &n = node(object);
  auto *state = atk_state_set_new();
  if (!live(n)) {
    atk_state_set_add_state(state, ATK_STATE_DEFUNCT);
    return state;
  }
  auto *w = n.widget();
  if (!w || w->visible()) atk_state_set_add_state(state, ATK_STATE_VISIBLE);
  if (showing(n)) atk_state_set_add_state(state, ATK_STATE_SHOWING);
  if (enabled(n)) {
    atk_state_set_add_state(state, ATK_STATE_ENABLED);
    atk_state_set_add_state(state, ATK_STATE_SENSITIVE);
  }
  if (n.kind == Kind::widget && w) {
    if (auto *window = dynamic_cast<Fl_Window *>(w)) {
      if (window->modal()) atk_state_set_add_state(state, ATK_STATE_MODAL);
      if (Fl::focus() && Fl::focus()->top_window() == window)
        atk_state_set_add_state(state, ATK_STATE_ACTIVE);
      if (window->resizable()) atk_state_set_add_state(state, ATK_STATE_RESIZABLE);
    } else if (w->visible_focus() && !dynamic_cast<Fl_Group *>(w)) {
      atk_state_set_add_state(state, ATK_STATE_FOCUSABLE);
    } else if (dynamic_cast<Fl_Browser *>(w)) {
      atk_state_set_add_state(state, ATK_STATE_FOCUSABLE);
    }
    if (auto *field = dynamic_cast<Fl_Input_ *>(w)) {
      if (!field->readonly()) atk_state_set_add_state(state, ATK_STATE_EDITABLE);
      atk_state_set_add_state(state, field->input_type() & FL_MULTILINE_INPUT
                                         ? ATK_STATE_MULTI_LINE : ATK_STATE_SINGLE_LINE);
    }
    if (dynamic_cast<Fl_Check_Button *>(w)) atk_state_set_add_state(state, ATK_STATE_CHECKABLE);
    if (dynamic_cast<Fl_Tabs *>(w->parent())) atk_state_set_add_state(state, ATK_STATE_SELECTABLE);
  }
  if (n.kind == Kind::row || n.kind == Kind::menu_item)
    atk_state_set_add_state(state, ATK_STATE_SELECTABLE);
  if (n.kind == Kind::menu_item) atk_state_set_add_state(state, ATK_STATE_FOCUSABLE);
  if (const auto *item = menu_item(n); item && (item->flags & (FL_MENU_TOGGLE | FL_MENU_RADIO)))
    atk_state_set_add_state(state, ATK_STATE_CHECKABLE);
  if (checked(n)) atk_state_set_add_state(state, ATK_STATE_CHECKED);
  if (selected(n)) atk_state_set_add_state(state, ATK_STATE_SELECTED);
  if (focused(n)) atk_state_set_add_state(state, ATK_STATE_FOCUSED);
  return state;
}
void finalize(GObject *object) {
  delete reinterpret_cast<LayerCSemantic *>(object)->node;
  G_OBJECT_CLASS(semantic_parent_class)->finalize(object);
}
void semantic_class_init(LayerCSemanticClass *type) {
  G_OBJECT_CLASS(type)->finalize = finalize;
  auto *object = ATK_OBJECT_CLASS(type);
  object->get_name = get_name;
  object->get_description = get_description;
  object->get_parent = get_parent;
  object->get_n_children = get_n_children;
  object->ref_child = ref_child;
  object->get_index_in_parent = index_in_parent;
  object->ref_state_set = ref_states;
}
void semantic_init(LayerCSemantic *object) { object->node = new Node; }
void text_semantic_class_init(LayerCTextSemanticClass *) {}
void text_semantic_init(LayerCTextSemantic *) {}
void action_semantic_class_init(LayerCActionSemanticClass *) {}
void action_semantic_init(LayerCActionSemantic *) {}
void value_semantic_class_init(LayerCValueSemanticClass *) {}
void value_semantic_init(LayerCValueSemantic *) {}
void selection_semantic_class_init(LayerCSelectionSemanticClass *) {}
void selection_semantic_init(LayerCSelectionSemantic *) {}
void window_semantic_class_init(LayerCWindowSemanticClass *) {}
void window_semantic_init(LayerCWindowSemantic *) {}

void defer(Fl_Widget *w, std::function<void(Fl_Widget &)> step);

void extents(AtkComponent *object, gint *x, gint *y, gint *width, gint *height,
             AtkCoordType coordinates) {
  auto &n = node(object);
  *x = *y = *width = *height = 0;
  auto *w = live(n) ? n.widget() : nullptr;
  if (!w) return;
  auto *window = w->top_window();
  if (!window || !window->shown()) return;
  // ContextBrowser owns the scroll-aware visible row geometry. Menu items and
  // plain browsers retain their owner bounds; never infer row pitch here.
  int left = w->x(), top_y = w->y(), width_units = w->w(), height_units = w->h();
  if (n.kind == Kind::row)
    if (auto *list = dynamic_cast<ContextBrowser *>(w)) {
      const auto bounds = list->row_bounds(n.index);
      if (!bounds) return;
      left = bounds->x;
      top_y = bounds->y;
      width_units = bounds->width;
      height_units = bounds->height;
    }
  auto bounds = n.kind == Kind::menu_item ? popup_item_bounds(*menu_owner(n), n.index)
      : w == window ? std::optional<EditorBounds>{{0, 0, w->w(), w->h()}}
                    : visible_bounds(*w, {left, top_y, width_units, height_units});
  if (!bounds) return;
  left = bounds->x; top_y = bounds->y;
  width_units = bounds->width; height_units = bounds->height;
  const float scale = Fl::screen_scale(window->screen_num());
  const bool top = w == window && n.kind != Kind::menu_item;
  *x = top ? 0 : static_cast<gint>(std::lround(left * scale));
  *y = top ? 0 : static_cast<gint>(std::lround(top_y * scale));
  *width = static_cast<gint>(std::lround(width_units * scale));
  *height = static_cast<gint>(std::lround(height_units * scale));
  if (coordinates == ATK_XY_SCREEN) {
    auto *display = fl_x11_display();
    const auto xid = fl_x11_xid(window);
    int origin_x{}, origin_y{};
    Window child{};
    if (!display || !xid ||
        !XTranslateCoordinates(display, xid, DefaultRootWindow(display), 0, 0, &origin_x,
                               &origin_y, &child)) {
      *x = *y = *width = *height = 0;
      return;
    }
    *x += origin_x;
    *y += origin_y;
  } else if (coordinates == ATK_XY_PARENT && !top) {
    auto *parent = n.parent ? node(n.parent).widget() : nullptr;
    if (parent && parent != window && parent->top_window() == window) {
      *x -= static_cast<gint>(std::lround(parent->x() * scale));
      *y -= static_cast<gint>(std::lround(parent->y() * scale));
    }
  }
}
gboolean grab_focus(AtkComponent *object) {
  auto &n = node(object);
  auto *w = n.widget();
  if (!w || !showing(n) || !enabled(n) || n.kind == Kind::menu_item) return FALSE;
  // take_focus() runs unfocus/focus handlers that may open a modal; like
  // every action, it runs from FLTK's loop rather than inside this call.
  const bool accepted = n.kind == Kind::row || w->visible_focus();
  if (!accepted) return FALSE;
  defer(w, [](Fl_Widget &target) { (void)target.take_focus(); });
  if (n.kind == Kind::row) {
    defer(w, [row = n.index](Fl_Widget &target) {
      auto &list = static_cast<Fl_Browser &>(target);
      if (row > list.size()) return;
      list.value(row);
      list.do_callback();
    });
  }
  w->redraw();
  return accepted;
}
void component_init(AtkComponentIface *type) {
  type->get_extents = extents;
  type->grab_focus = grab_focus;
  type->get_layer = [](AtkComponent *) { return ATK_LAYER_WIDGET; };
  type->get_alpha = [](AtkComponent *) { return 1.0; };
}

// AT-SPI calls arrive inside the adaptor's GLib dispatch. Application
// callbacks may open modal dialogs, so, like GTK, they run from FLTK's own
// loop after the call has returned; a deleted widget cancels its step.
namespace {
std::vector<std::function<void()>> deferred;
}
void run_deferred(void *) {
  auto steps = std::move(deferred);
  deferred.clear();
  for (auto &step : steps) step();
}
void defer(Fl_Widget *w, std::function<void(Fl_Widget &)> step) {
  auto tracker = std::make_shared<Fl_Widget_Tracker>(w);
  if (deferred.empty()) Fl::add_timeout(0.0, run_deferred);
  deferred.push_back([tracker, step = std::move(step)] {
    if (auto *live_widget = tracker->widget();
        live_widget && live_widget->active_r() && modal_allows(*live_widget))
      step(*live_widget);
  });
}

// Actions invoke the application's own callbacks; no input is synthesized.
gboolean activate(AtkObject *object) {
  auto &n = node(object);
  auto *w = n.widget();
  if (!w || !enabled(n) || (n.kind == Kind::menu_item ? !w->visible_r() : !showing(n))) return FALSE;
  const auto kind = n.kind;
  const int index = n.index;
  if (kind == Kind::menu_item) {
    const auto *item = menu_item(n);
    if (!item) return FALSE;
  }
  defer(w, [kind, index](Fl_Widget &target) {
    if (kind == Kind::row) {
      auto &list = static_cast<Fl_Browser &>(target);
      if (index > list.size()) return;
      list.value(index);
      list.do_callback();
      list.redraw();
    } else if (kind == Kind::menu_item) {
      auto &menu = static_cast<Fl_Menu_ &>(target);
      if (index >= menu_size(menu) - 1 || !menu.menu()[index].activevisible()) return;
      if (popup_pick(menu, index)) return;
      if (menu.menu()[index].submenu()) track_editor_popup(menu, index);
      else (void)menu.picked(&menu.menu()[index]);
    } else if (auto *tabs = dynamic_cast<Fl_Tabs *>(target.parent())) {
      if (tabs->value(&target)) tabs->do_callback();
      tabs->redraw();
    } else if (auto *button = dynamic_cast<Fl_Button *>(&target);
               button && button->type() == FL_RADIO_BUTTON) {
      button->setonly();
      button->do_callback();
    } else if (button && button->type() == FL_TOGGLE_BUTTON) {
      button->value(!button->value());
      button->do_callback();
    } else {
      target.do_callback();
    }
  });
  return TRUE;
}
gint action_count(AtkAction *object) {
  auto &n = node(object);
  if (!live(n)) return 0;
  const auto *item = menu_item(n);
  return n.kind == Kind::menu_item && !item ? 0 : 1;
}
const gchar *action_name(AtkAction *object, gint index) {
  if (index != 0) return nullptr;
  auto &n = node(object);
  if (n.kind == Kind::row || n.kind == Kind::menu_item) return "select";
  return dynamic_cast<Fl_Check_Button *>(n.widget()) ? "toggle" : "activate";
}
const gchar *action_keybinding(AtkAction *object, gint index) {
  if (index != 0) return nullptr;
  auto &n = node(object);
  n.keybinding = current_keybinding(n);
  return n.keybinding.c_str();
}
void action_init(AtkActionIface *type) {
  type->get_n_actions = action_count;
  type->do_action = [](AtkAction *object, gint index) -> gboolean {
    return index == 0 && activate(ATK_OBJECT(object));
  };
  type->get_name = action_name;
  type->get_description = [](AtkAction *object, gint index) -> const gchar * {
    return action_name(object, index) ? "Activate this control" : nullptr;
  };
  type->get_keybinding = action_keybinding;
}

gint char_count(const std::string &text) {
  return static_cast<gint>(g_utf8_strlen(text.c_str(), -1));
}
gint byte_offset(const std::string &text, gint offset) {
  offset = std::clamp(offset, 0, char_count(text));
  return static_cast<gint>(g_utf8_offset_to_pointer(text.c_str(), offset) - text.c_str());
}
gchar *text_range(AtkText *object, gint start, gint end) {
  const auto text = input_text(object);
  const auto count = char_count(text);
  if (start < 0 || start > count || (end != -1 && end < start)) return g_strdup("");
  const auto first = byte_offset(text, start);
  const auto last = end < 0 ? static_cast<gint>(text.size()) : byte_offset(text, end);
  return g_strndup(text.data() + first, static_cast<gsize>(last - first));
}
gint caret(AtkText *object) {
  auto *w = input(object);
  return w && w->value() ? static_cast<gint>(g_utf8_strlen(w->value(), w->insert_position())) : 0;
}
gboolean set_caret(AtkText *object, gint offset) {
  auto *w = input(object);
  if (!w || offset < 0 || offset > char_count(input_text(object))) return FALSE;
  w->insert_position(byte_offset(w->value(), offset));
  w->redraw();
  return TRUE;
}
gunichar char_at(const std::string &text, gint offset) {
  return g_utf8_get_char(text.c_str() + byte_offset(text, offset));
}
gchar *string_at(AtkText *object, gint offset, AtkTextGranularity granularity, gint *start,
                 gint *end) {
  const auto text = input_text(object);
  const auto count = char_count(text);
  *start = *end = 0;
  if (offset < 0 || offset >= count) return g_strdup("");
  *start = offset;
  *end = offset + 1;
  if (granularity == ATK_TEXT_GRANULARITY_WORD) {
    const auto space = [&](gint i) { return g_unichar_isspace(char_at(text, i)); };
    while (*start > 0 && !space(*start - 1)) --*start;
    while (*end < count && !space(*end)) ++*end;
  } else if (granularity != ATK_TEXT_GRANULARITY_CHAR) {
    // Lines, sentences and paragraphs all end at a newline in these fields.
    while (*start > 0 && char_at(text, *start - 1) != '\n') --*start;
    while (*end < count && char_at(text, *end - 1) != '\n') ++*end;
  }
  return text_range(object, *start, *end);
}
gint selection_bounds(Fl_Input_ *w, gint *start, gint *end) {
  *start = *end = 0;
  if (!w || !w->value() || w->insert_position() == w->mark()) return 0;
  *start = static_cast<gint>(g_utf8_strlen(w->value(), std::min(w->insert_position(), w->mark())));
  *end = static_cast<gint>(g_utf8_strlen(w->value(), std::max(w->insert_position(), w->mark())));
  return 1;
}
void text_init(AtkTextIface *type) {
  type->get_text = text_range;
  type->get_character_count = [](AtkText *object) { return char_count(input_text(object)); };
  type->get_character_at_offset = [](AtkText *object, gint offset) -> gunichar {
    const auto text = input_text(object);
    return offset >= 0 && offset < char_count(text) ? char_at(text, offset) : 0;
  };
  type->get_caret_offset = caret;
  type->set_caret_offset = set_caret;
  type->get_string_at_offset = string_at;
  type->get_n_selections = [](AtkText *object) -> gint {
    gint start{}, end{};
    return selection_bounds(input(object), &start, &end);
  };
  type->get_selection = [](AtkText *object, gint index, gint *start, gint *end) -> gchar * {
    if (index != 0 || !selection_bounds(input(object), start, end)) return nullptr;
    return text_range(object, *start, *end);
  };
  type->set_selection = [](AtkText *object, gint index, gint start, gint end) -> gboolean {
    auto *w = input(object);
    if (!w || index != 0 || start < 0 || end < start || end > char_count(input_text(object)))
      return FALSE;
    w->insert_position(byte_offset(w->value(), end), byte_offset(w->value(), start));
    w->redraw();
    return TRUE;
  };
}
// Edits go through the field's own callback, exactly like typed text.
Fl_Input_ *editable(void *object) {
  auto *w = input(object);
  // Offset edits on text that is not valid UTF-8 would not match what was exported.
  return w && !w->readonly() && enabled(node(object)) && showing(node(object)) &&
                 g_utf8_validate(w->value(), -1, nullptr)
             ? w
             : nullptr;
}
void editable_init(AtkEditableTextIface *type) {
  type->set_text_contents = [](AtkEditableText *object, const gchar *text) {
    auto *w = editable(object);
    if (!w || !text || !g_utf8_validate(text, -1, nullptr)) return;
    w->value(text);
    w->redraw();
    defer(w, [](Fl_Widget &target) { target.do_callback(); });
  };
  type->insert_text = [](AtkEditableText *object, const gchar *text, gint length, gint *position) {
    auto *w = editable(object);
    if (!w || !text || !position || length < 0 || !g_utf8_validate(text, length, nullptr)) return;
    auto value = input_text(object);
    if (*position < 0 || *position > char_count(value)) return;
    const auto at = byte_offset(value, *position);
    value.insert(static_cast<std::size_t>(at), text, static_cast<std::size_t>(length));
    // value() never runs callbacks; replace() would run FL_WHEN_CHANGED ones
    // inside this D-Bus call, before the deferred callback repeats them.
    w->value(value.c_str(), static_cast<int>(value.size()));
    w->insert_position(at + length);
    w->redraw();
    *position += static_cast<gint>(g_utf8_strlen(text, length));
    defer(w, [](Fl_Widget &target) { target.do_callback(); });
  };
  type->delete_text = [](AtkEditableText *object, gint start, gint end) {
    auto *w = editable(object);
    auto value = input_text(object);
    if (!w || start < 0 || end < start || end > char_count(value)) return;
    const auto first = byte_offset(value, start);
    value.erase(static_cast<std::size_t>(first),
                static_cast<std::size_t>(byte_offset(value, end) - first));
    w->value(value.c_str(), static_cast<int>(value.size()));
    w->insert_position(first);
    w->redraw();
    defer(w, [](Fl_Widget &target) { target.do_callback(); });
  };
}

Fl_Valuator *valuator(void *object) {
  auto &n = node(object);
  return live(n) ? dynamic_cast<Fl_Valuator *>(n.widget()) : nullptr;
}
void numeric_value(GValue *value, double number) {
  g_value_init(value, G_TYPE_DOUBLE);
  g_value_set_double(value, number);
}
void value_init(AtkValueIface *type) {
  type->get_current_value = [](AtkValue *object, GValue *value) {
    auto *w = valuator(object);
    numeric_value(value, w ? w->value() : 0.0);
  };
  type->get_minimum_value = [](AtkValue *object, GValue *value) {
    auto *w = valuator(object);
    numeric_value(value, w ? std::min(w->minimum(), w->maximum()) : 0.0);
  };
  type->get_maximum_value = [](AtkValue *object, GValue *value) {
    auto *w = valuator(object);
    numeric_value(value, w ? std::max(w->minimum(), w->maximum()) : 0.0);
  };
  type->get_minimum_increment = [](AtkValue *object, GValue *value) {
    auto *w = valuator(object);
    numeric_value(value, w ? w->step() : 0.0);
  };
  type->set_current_value = [](AtkValue *object, const GValue *value) -> gboolean {
    auto *w = valuator(object);
    if (!w || !enabled(node(object)) || !showing(node(object)) || !G_VALUE_HOLDS_DOUBLE(value))
      return FALSE;
    const double number = g_value_get_double(value);
    if (!std::isfinite(number) || number < std::min(w->minimum(), w->maximum()) ||
        number > std::max(w->minimum(), w->maximum()))
      return FALSE;
    if (w->value(number)) defer(w, [](Fl_Widget &target) { target.do_callback(); });
    w->redraw();
    return TRUE;
  };
}

// Lists, choices and tab strips expose their current child as the selection.
AtkObject *selected_child(void *object, gint *position = nullptr) {
  auto &children = node(object).children;
  for (std::size_t i = 0; i < children.size(); ++i)
    if (selected(node(children[i]))) {
      if (position) *position = static_cast<gint>(i);
      return children[i];
    }
  return nullptr;
}
void selection_init(AtkSelectionIface *type) {
  type->add_selection = [](AtkSelection *object, gint index) -> gboolean {
    auto &children = node(object).children;
    if (!live(node(object)) || index < 0 || static_cast<std::size_t>(index) >= children.size())
      return FALSE;
    return activate(children[static_cast<std::size_t>(index)]);
  };
  type->clear_selection = [](AtkSelection *) -> gboolean { return FALSE; };
  type->ref_selection = [](AtkSelection *object, gint index) -> AtkObject * {
    auto *child = index == 0 ? selected_child(object) : nullptr;
    return child ? ATK_OBJECT(g_object_ref(child)) : nullptr;
  };
  type->get_selection_count = [](AtkSelection *object) -> gint {
    return selected_child(object) ? 1 : 0;
  };
  type->is_child_selected = [](AtkSelection *object, gint index) -> gboolean {
    gint position{-1};
    return selected_child(object, &position) && position == index;
  };
  type->remove_selection = [](AtkSelection *, gint) -> gboolean { return FALSE; };
  type->select_all_selection = [](AtkSelection *) -> gboolean { return FALSE; };
}

namespace {
AtkObject *root_object{};
AtkObject *get_root() { return root_object; }
const gchar *toolkit_name() { return "FLTK"; }
const gchar *toolkit_version() { return "1.4"; }
}

bool root_registered() noexcept { return root_object != nullptr; }
void register_root(AtkObject *root) {
  root_object = root;
  auto *util = static_cast<AtkUtilClass *>(g_type_class_ref(ATK_TYPE_UTIL));
  util->get_root = get_root;
  util->get_toolkit_name = toolkit_name;
  util->get_toolkit_version = toolkit_version;
  g_type_class_unref(util);
}
void clear_root() noexcept { root_object = nullptr; }
void cancel_deferred() {
  Fl::remove_timeout(run_deferred);
  deferred.clear();
}

bool same(const Node &n, const Key &key) {
  return live(n) && n.kind == key.kind && n.widget() == key.widget && n.index == key.index;
}
// Popup menu buttons and unlabeled decoration are not separate semantics.
bool exported(Fl_Widget *w) {
  if (w == popup_owner()) return true;
  if (w->w() <= 0 || w->h() <= 0) return false;
  if (dynamic_cast<Fl_Box *>(w) && !(w->label() && *w->label())) return false;
  return !(dynamic_cast<Fl_Menu_ *>(w) && !dynamic_cast<Fl_Menu_Bar *>(w) &&
           !dynamic_cast<Fl_Choice *>(w));
}
void remember(AtkObject *object) {
  auto &n = node(object);
  n.name = current_name(n);
  n.description = current_description(object);
  n.focused = focused(n);
  n.selected = selected(n);
  n.checked = checked(n);
  n.showing = showing(n);
  n.enabled = enabled(n);
  n.text = input_text(object);
  n.caret = ATK_IS_TEXT(object) ? caret(ATK_TEXT(object)) : -1;
}

AtkObject *make(const Key &key, AtkObject *parent) {
  GType type = semantic_get_type();
  AtkRole role = ATK_ROLE_PANEL;
  auto *w = key.widget;
  if (key.kind == Kind::application) {
    role = ATK_ROLE_APPLICATION;
  } else if (key.kind == Kind::row) {
    type = action_semantic_get_type();
    role = ATK_ROLE_LIST_ITEM;
  } else if (key.kind == Kind::menu_item) {
    auto *menu = static_cast<Fl_Menu_ *>(w);
    const auto *item = &menu->menu()[key.index];
    type = action_semantic_get_type();
    role = item->submenu() ? ATK_ROLE_MENU
           : (item->flags & FL_MENU_TOGGLE) ? ATK_ROLE_CHECK_MENU_ITEM
           : (item->flags & FL_MENU_RADIO) ? ATK_ROLE_RADIO_MENU_ITEM
                                            : ATK_ROLE_MENU_ITEM;
  } else if (auto *window = dynamic_cast<Fl_Window *>(w)) {
    type = window_semantic_get_type();
    role = window->non_modal() ? ATK_ROLE_DIALOG : ATK_ROLE_FRAME;
  } else if (auto *field = dynamic_cast<Fl_Input_ *>(w)) {
    type = text_semantic_get_type();
    role = field->readonly() ? ATK_ROLE_TEXT : ATK_ROLE_ENTRY;
  } else if (dynamic_cast<Fl_Valuator *>(w)) {
    type = value_semantic_get_type();
    role = dynamic_cast<Fl_Scrollbar *>(w) ? ATK_ROLE_SCROLL_BAR
         : dynamic_cast<Fl_Value_Input *>(w) ? ATK_ROLE_SPIN_BUTTON
         : dynamic_cast<Fl_Slider *>(w) ? ATK_ROLE_SLIDER : ATK_ROLE_SPIN_BUTTON;
  } else if (dynamic_cast<Fl_Menu_Bar *>(w)) {
    role = ATK_ROLE_MENU_BAR;
  } else if (dynamic_cast<Fl_Choice *>(w)) {
    type = selection_semantic_get_type();
    role = ATK_ROLE_COMBO_BOX;
  } else if (dynamic_cast<Fl_Menu_ *>(w)) {
    type = selection_semantic_get_type();
    role = ATK_ROLE_MENU;
  } else if (dynamic_cast<Fl_Browser *>(w)) {
    type = selection_semantic_get_type();
    role = ATK_ROLE_LIST;
  } else if (dynamic_cast<Fl_Tabs *>(w)) {
    type = selection_semantic_get_type();
    role = ATK_ROLE_PAGE_TAB_LIST;
  } else if (dynamic_cast<Fl_Tabs *>(w->parent())) {
    type = action_semantic_get_type();
    role = ATK_ROLE_PAGE_TAB;
  } else if (dynamic_cast<Fl_Check_Button *>(w)) {
    type = action_semantic_get_type();
    role = ATK_ROLE_CHECK_BOX;
  } else if (dynamic_cast<Fl_Button *>(w)) {
    type = action_semantic_get_type();
    role = ATK_ROLE_PUSH_BUTTON;
  } else if (dynamic_cast<Fl_Box *>(w)) {
    role = ATK_ROLE_LABEL;
  } else if (dynamic_cast<Fl_Scroll *>(w)) {
    role = ATK_ROLE_SCROLL_PANE;
  } else if (dynamic_cast<PanelPreview *>(w) || dynamic_cast<CalibrationPreview *>(w)) {
    role = ATK_ROLE_CANVAS;
  }
  auto *object = ATK_OBJECT(g_object_new(type, nullptr));
  auto &n = node(object);
  n.kind = key.kind;
  n.index = key.index;
  n.parent = parent;
  if (w) n.tracker = std::make_unique<Fl_Widget_Tracker>(w);
  atk_object_set_role(object, role);
  atk_object_set_parent(object, parent);
  if (dynamic_cast<PanelPreview *>(w))
    atk_object_set_description(object, "Drag a panel to change its anchor. "
                                       "The Panel fields edit the same values.");
  remember(object);
  return object;
}
std::vector<Key> desired(AtkObject *object) {
  auto &n = node(object);
  std::vector<Key> keys;
  auto *w = n.widget();
  if (n.kind == Kind::application) {
    std::vector<Fl_Window *> shown;
    for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
      if (!window->parent() && !window->menu_window() && !window->tooltip_window())
        shown.push_back(window);
    // Keep known windows in place; FLTK reorders its list on activation.
    for (auto *child : n.children)
      if (auto *window = node(child).widget();
          window && std::find(shown.begin(), shown.end(), window) != shown.end())
        keys.push_back({window, Kind::widget, 0});
    for (auto *window : shown)
      if (std::none_of(keys.begin(), keys.end(), [&](const Key &k) { return k.widget == window; }))
        keys.push_back({window, Kind::widget, 0});
  } else if (n.kind == Kind::menu_item) {
    const auto *item = menu_item(n);
    if (item && (item->flags & FL_SUBMENU))
      for (const int index : menu_level(*menu_owner(n), n.index + 1))
        keys.push_back({w, Kind::menu_item, index});
  } else if (n.kind == Kind::widget && w) {
    if (auto *list = dynamic_cast<Fl_Browser *>(w)) {
      // Hidden pages have no reachable rows. Materialize every row when the
      // browser is shown; scrolling must not restrict accessible children.
      if (list->visible_r())
        for (int row = 1; row <= list->size(); ++row) keys.push_back({w, Kind::row, row});
    } else if (auto *menu = dynamic_cast<Fl_Menu_ *>(w)) {
      for (const int index : menu_level(*menu, 0)) keys.push_back({w, Kind::menu_item, index});
    } else if (auto *group = w->as_group(); group && !dynamic_cast<Fl_Input_ *>(w)) {
      for (int i = 0; i < group->children(); ++i)
        if (exported(group->child(i))) keys.push_back({group->child(i), Kind::widget, 0});
    }
  }
  return keys;
}

} // namespace layerc::editor::accessibility
