// FLTK semantics exported through ATK and its AT-SPI adaptor, without GTK.
#include "editor_accessibility.hpp"
#include "editor_app.hpp"
#include "calibration_window.hpp"

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
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <atk-bridge.h>
#include <atk/atk.h>
#include <atspi/atspi.h>
#include <link.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor {
namespace {
// ATK, its adaptor and libatspi ship together from at-spi2-core. Releases
// before 2.62 crash in the adaptor's cache cleanup (GNOME at-spi2-core
// c4ddb104, 8db3f580), so every loaded copy must be one coherent >=2.62 set.
std::string bridge_closure_fault() {
  struct Loaded {
    std::string atk, bridge, atspi;
    int copies{};
  } loaded;
  dl_iterate_phdr(
      [](dl_phdr_info *info, std::size_t, void *data) {
        auto &found = *static_cast<Loaded *>(data);
        const std::string_view path = info->dlpi_name ? info->dlpi_name : "";
        const auto name = path.substr(path.rfind('/') + 1);
        if (name.starts_with("libatk-1.0.so")) found.atk = path, ++found.copies;
        else if (name.starts_with("libatk-bridge-2.0.so")) found.bridge = path, ++found.copies;
        else if (name.starts_with("libatspi.so")) found.atspi = path, ++found.copies;
        return 0;
      },
      &loaded);
  if (loaded.atk.empty() || loaded.bridge.empty() || loaded.atspi.empty())
    return "the ATK/AT-SPI libraries are not all loaded";
  if (loaded.copies != 3) return "more than one copy of an ATK/AT-SPI library is loaded";
  const auto directory = [](const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return error ? std::string() : canonical.parent_path().string();
  };
  const auto home = directory(loaded.atk);
  if (home.empty() || directory(loaded.bridge) != home || directory(loaded.atspi) != home)
    return "the ATK/AT-SPI libraries come from different installations";
  gint major{}, minor{}, micro{};
  atspi_get_version(&major, &minor, &micro);
  const auto old = [](guint a, guint b) { return a < 2 || (a == 2 && b < 62); };
  if (old(atk_get_major_version(), atk_get_minor_version()) ||
      old(static_cast<guint>(major), static_cast<guint>(minor)))
    return "ATK/AT-SPI 2.62 or later is required";
  return {};
}

enum class Kind { application, widget, row, menu_item };
struct Key {
  Fl_Widget *widget;
  Kind kind;
  int index;
};

struct Node {
  std::unique_ptr<Fl_Widget_Tracker> tracker;
  AtkObject *parent{};
  std::vector<AtkObject *> children; // Each holds this node's reference.
  Kind kind{Kind::application};
  int index{}; // Browser row (1-based) or menu item index.
  bool retired{};
  // Last state reported to the adaptor; changes become ATK events.
  std::string name, text, keybinding;
  std::string name_reply; // get_name's buffer; never the reported name.
  int caret{-1};
  bool focused{}, selected{}, checked{}, showing{}, enabled{};
  Fl_Widget *widget() const { return tracker ? tracker->widget() : nullptr; }
};

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

Fl_Menu_ *menu_owner(const Node &n) { return dynamic_cast<Fl_Menu_ *>(n.widget()); }
const Fl_Menu_Item *menu_item(const Node &n) {
  auto *menu = menu_owner(n);
  if (n.kind != Kind::menu_item || !menu || !menu->menu() || n.index < 0 ||
      n.index >= menu->size() - 1)
    return nullptr;
  return &menu->menu()[n.index];
}
// Visible items of one inline menu level; FL_SUBMENU_POINTER is not used.
std::vector<int> menu_level(const Fl_Menu_ &menu, int first) {
  std::vector<int> items;
  const auto *array = menu.menu();
  if (!array || first < 0 || first >= menu.size() - 1) return items;
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
    auto *choice = dynamic_cast<Fl_Choice *>(w);
    return choice && choice->value() == n.index;
  }
  if (auto *tabs = dynamic_cast<Fl_Tabs *>(w->parent())) return tabs->value() == w;
  return false;
}
bool focused(const Node &n) {
  auto *w = n.widget();
  if (!w || !live(n) || n.kind == Kind::menu_item || Fl::focus() != w) return false;
  // A focused list reports its current row, as native toolkits do.
  if (n.kind == Kind::row) return static_cast<Fl_Browser *>(w)->value() == n.index;
  return dynamic_cast<Fl_Browser *>(w) == nullptr;
}
bool showing(const Node &n) {
  if (!live(n)) return false;
  auto *w = n.widget();
  if (!w) return true;
  // A page tab is visible whenever its tab strip is, selected or not.
  if (n.kind == Kind::widget && dynamic_cast<Fl_Tabs *>(w->parent()))
    return w->parent()->visible_r() != 0;
  return w->visible_r() != 0;
}
bool modal_allows(Fl_Widget &widget) {
  const auto *modal = Fl::modal();
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
// Answers from its own buffer: overwriting the last reported name here would
// hide the change from report_changes and leave other clients' caches stale.
const gchar *get_name(AtkObject *object) {
  auto &n = node(object);
  n.name_reply = current_name(n);
  return n.name_reply.c_str();
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
  if (n.kind == Kind::row || (n.kind == Kind::menu_item && dynamic_cast<Fl_Choice *>(w)))
    atk_state_set_add_state(state, ATK_STATE_SELECTABLE);
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
  // Rows and menu items report their owner's bounds. Coordinates are scaled
  // FLTK units translated from OUR client origin, never a foreign window.
  const float scale = Fl::screen_scale(window->screen_num());
  const bool top = w == window;
  *x = top ? 0 : static_cast<gint>(std::lround(w->x() * scale));
  *y = top ? 0 : static_cast<gint>(std::lround(w->y() * scale));
  *width = static_cast<gint>(std::lround(w->w() * scale));
  *height = static_cast<gint>(std::lround(w->h() * scale));
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
  } else if (coordinates == ATK_XY_PARENT && n.kind == Kind::widget && !top) {
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
std::vector<std::function<void()>> deferred;
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
  if (!w || !enabled(n) || !showing(n)) return FALSE;
  const auto kind = n.kind;
  const int index = n.index;
  if (kind == Kind::menu_item) {
    const auto *item = menu_item(n);
    if (!item || item->submenu()) return FALSE;
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
      if (index >= menu.size() - 1 || !menu.menu()[index].activevisible()) return;
      (void)menu.picked(&menu.menu()[index]);
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
  return n.kind == Kind::menu_item && (!item || item->submenu()) ? 0 : 1;
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

AtkObject *root_object{};
AtkObject *get_root() { return root_object; }
const gchar *toolkit_name() { return "FLTK"; }
const gchar *toolkit_version() { return "1.4"; }

bool same(const Node &n, const Key &key) {
  return live(n) && n.kind == key.kind && n.widget() == key.widget && n.index == key.index;
}
// Popup menu buttons and unlabeled decoration are not separate semantics.
bool exported(Fl_Widget *w) {
  if (w->w() <= 0 || w->h() <= 0) return false;
  if (dynamic_cast<Fl_Box *>(w) && !(w->label() && *w->label())) return false;
  return !(dynamic_cast<Fl_Menu_ *>(w) && !dynamic_cast<Fl_Menu_Bar *>(w) &&
           !dynamic_cast<Fl_Choice *>(w));
}
} // namespace

struct EditorAccessibility::Impl {
  EditorApp &app;
  AtkObject *root{};
  std::vector<GPollFD> descriptors;
  std::string fault_reason;
  bool initialized{};
  bool pumping{};
  bool gtk_chooser{};
  AtkObject *active_window{};
  GMainContext *context{g_main_context_default()};

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
      role = ATK_ROLE_SCROLL_BAR;
    } else if (dynamic_cast<Fl_Menu_Bar *>(w)) {
      role = ATK_ROLE_MENU_BAR;
    } else if (dynamic_cast<Fl_Choice *>(w)) {
      type = selection_semantic_get_type();
      role = ATK_ROLE_COMBO_BOX;
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
  void remember(AtkObject *object) {
    auto &n = node(object);
    n.name = current_name(n);
    n.focused = focused(n);
    n.selected = selected(n);
    n.checked = checked(n);
    n.showing = showing(n);
    n.enabled = enabled(n);
    n.text = input_text(object);
    n.caret = ATK_IS_TEXT(object) ? caret(ATK_TEXT(object)) : -1;
  }

  std::vector<Key> desired(AtkObject *object) const {
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

  void retire(AtkObject *object, bool quiet) {
    auto &n = node(object);
    for (auto *child : n.children) retire(child, quiet);
    n.children.clear();
    if (!quiet && ATK_IS_WINDOW(object)) g_signal_emit_by_name(object, "destroy");
    if (active_window == object) active_window = nullptr;
    n.retired = true;
    n.tracker.reset();
    n.parent = nullptr;
    atk_object_set_parent(object, nullptr);
    if (!quiet) atk_object_notify_state_change(object, ATK_STATE_DEFUNCT, TRUE);
    g_object_unref(object);
  }

  void reconcile(AtkObject *object, bool quiet) {
    auto &n = node(object);
    if (!live(n)) return;
    auto previous = n.children;
    std::vector<AtkObject *> next;
    std::vector<bool> added;
    for (const auto &key : desired(object)) {
      // Children usually keep their position; search only when they moved.
      const auto index = static_cast<std::ptrdiff_t>(next.size());
      const auto found = index < static_cast<std::ptrdiff_t>(previous.size()) &&
                                 previous[static_cast<std::size_t>(index)] &&
                                 same(node(previous[static_cast<std::size_t>(index)]), key)
                             ? previous.begin() + index
                             : std::find_if(previous.begin(), previous.end(), [&](AtkObject *child) {
                                 return child && same(node(child), key);
                               });
      if (found != previous.end()) {
        next.push_back(*found);
        *found = nullptr;
        added.push_back(false);
      } else {
        next.push_back(make(key, object));
        added.push_back(true);
      }
    }
    for (std::size_t i = previous.size(); i-- > 0;) {
      if (!previous[i]) continue;
      // Unlist first, so a listener never reaches a child being retired.
      n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(i));
      if (!quiet)
        g_signal_emit_by_name(object, "children-changed::remove", static_cast<gint>(i), previous[i]);
      retire(previous[i], quiet);
    }
    n.children = std::move(next);
    for (std::size_t i = 0; i < n.children.size(); ++i) {
      auto *child = n.children[i];
      if (added[i] && !quiet) {
        // Announce a new subtree once, already complete.
        reconcile(child, true);
        g_signal_emit_by_name(object, "children-changed::add", static_cast<gint>(i), child);
        if (ATK_IS_WINDOW(child)) g_signal_emit_by_name(child, "create");
      } else {
        reconcile(child, quiet);
      }
    }
    if (!quiet) report_changes(object);
  }

  void report_changes(AtkObject *object) {
    auto &n = node(object);
    if (!live(n)) return;
    if (auto name = current_name(n); name != n.name) {
      n.name = std::move(name);
      g_object_notify(G_OBJECT(object), "accessible-name");
    }
    const auto state = [&](bool &last, bool next, AtkStateType type) {
      if (next == last) return false;
      last = next;
      atk_object_notify_state_change(object, type, next);
      return true;
    };
    if (bool next = enabled(n); next != n.enabled) {
      state(n.enabled, next, ATK_STATE_ENABLED);
      atk_object_notify_state_change(object, ATK_STATE_SENSITIVE, next);
    }
    state(n.showing, showing(n), ATK_STATE_SHOWING);
    state(n.checked, checked(n), ATK_STATE_CHECKED);
    if (state(n.selected, selected(n), ATK_STATE_SELECTED) && n.selected && n.parent &&
        ATK_IS_SELECTION(n.parent))
      g_signal_emit_by_name(n.parent, "selection-changed");
    if (ATK_IS_TEXT(object)) {
      const auto text = input_text(object);
      if (text != n.text) report_text(object, n, text);
      if (const auto next = caret(ATK_TEXT(object)); next != n.caret) {
        n.caret = next;
        if (Fl::focus() == n.widget()) g_signal_emit_by_name(object, "text-caret-moved", next);
      }
    }
    // Focus is reported last, after the object's new name and state.
    state(n.focused, focused(n), ATK_STATE_FOCUSED);
  }

  // Reports only the changed span, with the removed text itself: the
  // adaptor cannot read removed text back from the updated field.
  static void report_text(AtkObject *object, Node &n, const std::string &text) {
    const auto old = std::move(n.text);
    n.text = text;
    const auto continuation = [](const std::string &s, std::size_t at) {
      return at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80;
    };
    std::size_t prefix = 0;
    while (prefix < old.size() && prefix < text.size() && old[prefix] == text[prefix]) ++prefix;
    while (prefix && (continuation(old, prefix) || continuation(text, prefix))) --prefix;
    std::size_t suffix = 0;
    while (suffix < old.size() - prefix && suffix < text.size() - prefix &&
           old[old.size() - 1 - suffix] == text[text.size() - 1 - suffix]) ++suffix;
    while (suffix && continuation(old, old.size() - suffix)) --suffix;
    const auto removed = old.substr(prefix, old.size() - prefix - suffix);
    const auto inserted = text.substr(prefix, text.size() - prefix - suffix);
    const auto position = static_cast<gint>(g_utf8_strlen(old.data(), static_cast<gssize>(prefix)));
    if (!removed.empty())
      g_signal_emit_by_name(object, "text-remove", position, char_count(removed), removed.c_str());
    if (!inserted.empty())
      g_signal_emit_by_name(object, "text-insert", position, char_count(inserted), inserted.c_str());
  }

  void report_window() {
    AtkObject *next{};
    Fl_Window *window = Fl::modal() ? Fl::modal() : (Fl::focus() ? Fl::focus()->top_window() : nullptr);
    for (auto *child : node(root).children)
      if (node(child).widget() == window) next = child;
    if (next == active_window) return;
    if (active_window) {
      atk_object_notify_state_change(active_window, ATK_STATE_ACTIVE, FALSE);
      g_signal_emit_by_name(active_window, "deactivate");
    }
    active_window = next;
    if (next) {
      atk_object_notify_state_change(next, ATK_STATE_ACTIVE, TRUE);
      g_signal_emit_by_name(next, "activate");
    }
  }

  explicit Impl(EditorApp &owner) : app(owner) {
    if (root_object) {
      fault_reason = "another LayerE window owns the accessibility bridge";
      return;
    }
    root = make({nullptr, Kind::application, 0}, nullptr);
    root_object = root;
    auto *util = static_cast<AtkUtilClass *>(g_type_class_ref(ATK_TYPE_UTIL));
    util->get_root = get_root;
    util->get_toolkit_name = toolkit_name;
    util->get_toolkit_version = toolkit_version;
    g_type_class_unref(util);
    if (const auto *disabled = g_getenv("NO_AT_BRIDGE"); disabled && std::string_view(disabled) == "1") {
      fault_reason = "disabled by NO_AT_BRIDGE=1";
      return;
    }
    if (auto closure = bridge_closure_fault(); !closure.empty()) {
      fault_reason = std::move(closure);
      return;
    }
    // The adaptor's init succeeds without a bus and exports nothing; check
    // first so the window shows why. The adaptor reuses this connection.
    if (!atspi_get_a11y_bus()) {
      fault_reason = "no accessibility bus is reachable";
      return;
    }
    // The whole tree exists before the adaptor registers its listeners.
    reconcile(root, true);
    if (atk_bridge_adaptor_init(nullptr, nullptr) != 0) {
      fault_reason = "no accessibility bus is reachable";
      return;
    }
    initialized = true;
    // GTK's file chooser would install a second ATK implementation in this
    // process; use FLTK's own chooser, whose widgets this bridge exports.
    gtk_chooser = Fl::option(Fl::OPTION_FNFC_USES_GTK);
    Fl::option(Fl::OPTION_FNFC_USES_GTK, false);
    atk_bridge_set_event_context(context);
    Fl::add_check(check, this);
    pump();
  }
  ~Impl() {
    // An instance refused because another owns the bridge holds no state.
    if (!root) return;
    Fl::remove_timeout(run_deferred);
    deferred.clear();
    Fl::remove_check(check, this);
    Fl::remove_timeout(wake, this);
    remove_descriptors();
    if (initialized) Fl::option(Fl::OPTION_FNFC_USES_GTK, gtk_chooser);
    // Keep accessibles alive while the adaptor retires its leases and weak
    // references. Detach the tree first so its cleanup sees an empty root.
    std::vector<AtkObject *> objects;
    collect(root, objects);
    for (auto *object : objects) {
      node(object).children.clear();
      node(object).parent = nullptr;
      atk_object_set_parent(object, nullptr);
    }
    if (initialized) atk_bridge_adaptor_cleanup();
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) g_object_unref(*it);
    root_object = nullptr;
  }
  static void collect(AtkObject *object, std::vector<AtkObject *> &objects) {
    objects.push_back(object);
    for (auto *child : node(object).children) collect(child, objects);
  }
  void remove_descriptors() {
    for (const auto &fd : descriptors) Fl::remove_fd(fd.fd, FL_READ | FL_WRITE | FL_EXCEPT);
    descriptors.clear();
  }
  static void ready(int, void *self) { static_cast<Impl *>(self)->pump(); }
  static void wake(void *self) { static_cast<Impl *>(self)->pump(); }
  static void check(void *self) { static_cast<Impl *>(self)->pump(); }
  void sync() {
    reconcile(root, false);
    report_window();
  }
  void pump() {
    if (pumping || !initialized || !g_main_context_acquire(context)) return;
    pumping = true;
    sync();
    // Integrate GLib's readiness descriptors/deadlines into FLTK's blocking
    // loop: no fixed-rate timer, busy idle callback or auxiliary UI thread.
    bool dispatched = false;
    for (unsigned i = 0; i < 64 && g_main_context_pending(context); ++i)
      dispatched = g_main_context_iteration(context, FALSE) || dispatched;
    if (dispatched) sync();
    remove_descriptors();
    gint priority = 0, timeout = -1;
    g_main_context_prepare(context, &priority);
    const gint count = g_main_context_query(context, priority, &timeout, nullptr, 0);
    descriptors.resize(static_cast<std::size_t>(std::max(count, 0)));
    g_main_context_query(context, priority, &timeout, descriptors.data(), count);
    for (const auto &fd : descriptors) {
      int events = 0;
      if (fd.events & G_IO_IN) events |= FL_READ;
      if (fd.events & G_IO_OUT) events |= FL_WRITE;
      if (fd.events & (G_IO_ERR | G_IO_HUP)) events |= FL_EXCEPT;
      if (events) Fl::add_fd(fd.fd, events, ready, this);
    }
    Fl::remove_timeout(wake, this);
    if (timeout >= 0) Fl::add_timeout(static_cast<double>(timeout) / 1000.0, wake, this);
    pumping = false;
    g_main_context_release(context);
  }
};

EditorAccessibility::EditorAccessibility(EditorApp &app) : impl_(std::make_unique<Impl>(app)) {}
EditorAccessibility::~EditorAccessibility() = default;
bool EditorAccessibility::available() const { return impl_->initialized; }
const std::string &EditorAccessibility::fault() const { return impl_->fault_reason; }
} // namespace layerc::editor
