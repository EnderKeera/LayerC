// Prototype only: FLTK semantics exported through ATK/AT-SPI, without GTK.
#include "fltk_a11y.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <atk/atk.h>
#include <atk-bridge.h>
#include <link.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
// ATK, its AT-SPI adaptor and libatspi must be one coherent, version-checked
// installation: an older system adaptor has a known cleanup defect. All three
// ship from one at-spi2-core source package; the canonical directory of each
// loaded copy must be the one configured (and version-checked) at build time.
// Returns a visible fault reason, or an empty string when the closure is valid.
std::string bridge_closure_fault() {
  struct Loaded {
    std::string atk, bridge, atspi;
  } loaded;
  dl_iterate_phdr(
      [](dl_phdr_info *info, std::size_t, void *data) {
        auto &found = *static_cast<Loaded *>(data);
        const std::string_view path = info->dlpi_name ? info->dlpi_name : "";
        const auto name = path.substr(path.rfind('/') + 1);
        if (name.starts_with("libatk-1.0.so")) found.atk = path;
        else if (name.starts_with("libatk-bridge-2.0.so")) found.bridge = path;
        else if (name.starts_with("libatspi.so")) found.atspi = path;
        return 0;
      },
      &loaded);
  if (loaded.atk.empty() || loaded.bridge.empty() || loaded.atspi.empty())
    return "bridge libraries are not all loaded";
  const auto directory = [](const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return error ? std::string() : canonical.parent_path().string();
  };
  for (const auto *path : {&loaded.atk, &loaded.bridge, &loaded.atspi})
    if (directory(*path) != LAYERC_PROTO_BRIDGE_DIR)
      return "bridge library " + *path + " is not from the configured installation";
  if (atk_get_major_version() < 2 || (atk_get_major_version() == 2 && atk_get_minor_version() < 62))
    return "ATK is older than the required 2.62";
  return {};
}

struct Node {
  std::unique_ptr<Fl_Widget_Tracker> tracker;
  AtkObject *parent{};
  std::vector<AtkObject *> children;
  std::string name;
  std::string text;
  std::string last_name;
  bool focused{};
  bool selected{};
  int row{}; // Synthetic browser row, 1-based; zero means a widget.
  Fl_Widget *widget() const { return tracker ? tracker->widget() : nullptr; }
};

struct Semantic { AtkObject parent; Node *node; };
struct SemanticClass { AtkObjectClass parent; };
struct InputSemantic { Semantic parent; };
struct InputSemanticClass { SemanticClass parent; };
struct ActionSemantic { Semantic parent; };
struct ActionSemanticClass { SemanticClass parent; };
struct ValueSemantic { Semantic parent; };
struct ValueSemanticClass { SemanticClass parent; };

void component_init(AtkComponentIface *);
void text_init(AtkTextIface *);
void editable_init(AtkEditableTextIface *);
void action_init(AtkActionIface *);
void value_init(AtkValueIface *);
G_DEFINE_TYPE_WITH_CODE(Semantic, semantic, ATK_TYPE_OBJECT,
                       G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, component_init))
G_DEFINE_TYPE_WITH_CODE(InputSemantic, input_semantic, semantic_get_type(),
                       G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, text_init)
                       G_IMPLEMENT_INTERFACE(ATK_TYPE_EDITABLE_TEXT, editable_init))
G_DEFINE_TYPE_WITH_CODE(ActionSemantic, action_semantic, semantic_get_type(),
                       G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, action_init))
G_DEFINE_TYPE_WITH_CODE(ValueSemantic, value_semantic, semantic_get_type(),
                       G_IMPLEMENT_INTERFACE(ATK_TYPE_VALUE, value_init))

Node &node(void *object) { return *reinterpret_cast<Semantic *>(object)->node; }
std::string clean_label(const char *label) {
  std::string result;
  if (label) for (const char *p = label; *p; ++p) {
    if (*p == '&' && p[1] != '&') continue;
    result += *p;
    if (*p == '&' && p[1] == '&') ++p;
  }
  return result;
}
bool live(Node &n) { return !n.tracker || n.widget() != nullptr; }
bool selected(Node &n) {
  auto *w = n.widget();
  if (!w) return false;
  if (n.row) return static_cast<Fl_Hold_Browser *>(w)->value() == n.row;
  if (auto *tabs = dynamic_cast<Fl_Tabs *>(w->parent())) return tabs->value() == w;
  return false;
}
bool focused(Node &n) {
  auto *w = n.widget();
  if (!w || Fl::focus() != w) return false;
  if (n.row) return selected(n);
  return dynamic_cast<Fl_Hold_Browser *>(w) == nullptr;
}
bool showing(Node &n) {
  auto *w = n.widget();
  if (!w) return !n.tracker;
  if (!n.row && dynamic_cast<Fl_Tabs *>(w->parent())) return w->parent()->visible_r();
  return w->visible_r();
}
const gchar *get_name(AtkObject *object) {
  auto &n = node(object);
  auto *w = n.widget();
  if (n.row && w) {
    const auto *text = static_cast<Fl_Hold_Browser *>(w)->text(n.row);
    return text ? text : "";
  }
  if (dynamic_cast<Fl_Box *>(w) && w->label() && *w->label()) return w->label();
  return n.name.c_str();
}
AtkObject *get_parent(AtkObject *object) { return node(object).parent; }
gint get_n_children(AtkObject *object) {
  return live(node(object)) ? static_cast<gint>(node(object).children.size()) : 0;
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
  if (!live(n)) { atk_state_set_add_state(state, ATK_STATE_DEFUNCT); return state; }
  auto *w = n.widget();
  atk_state_set_add_state(state, ATK_STATE_VISIBLE);
  if (showing(n)) atk_state_set_add_state(state, ATK_STATE_SHOWING);
  if (!w || w->active_r()) {
    atk_state_set_add_state(state, ATK_STATE_ENABLED);
    atk_state_set_add_state(state, ATK_STATE_SENSITIVE);
  }
  if (w && (dynamic_cast<Fl_Input *>(w) || dynamic_cast<Fl_Button *>(w) ||
            dynamic_cast<Fl_Spinner *>(w) || dynamic_cast<Fl_Hold_Browser *>(w)))
    atk_state_set_add_state(state, ATK_STATE_FOCUSABLE);
  if (dynamic_cast<Fl_Input *>(w)) {
    atk_state_set_add_state(state, ATK_STATE_EDITABLE);
    atk_state_set_add_state(state, ATK_STATE_SINGLE_LINE);
  }
  if (n.row || (w && dynamic_cast<Fl_Tabs *>(w->parent())))
    atk_state_set_add_state(state, ATK_STATE_SELECTABLE);
  if (selected(n)) atk_state_set_add_state(state, ATK_STATE_SELECTED);
  if (focused(n)) atk_state_set_add_state(state, ATK_STATE_FOCUSED);
  return state;
}
void finalize(GObject *object) {
  delete reinterpret_cast<Semantic *>(object)->node;
  G_OBJECT_CLASS(semantic_parent_class)->finalize(object);
}
void semantic_class_init(SemanticClass *type) {
  G_OBJECT_CLASS(type)->finalize = finalize;
  auto *object = ATK_OBJECT_CLASS(type);
  object->get_name = get_name;
  object->get_parent = get_parent;
  object->get_n_children = get_n_children;
  object->ref_child = ref_child;
  object->get_index_in_parent = index_in_parent;
  object->ref_state_set = ref_states;
}
void semantic_init(Semantic *object) { object->node = new Node; }
void input_semantic_class_init(InputSemanticClass *) {}
void input_semantic_init(InputSemantic *) {}
void action_semantic_class_init(ActionSemanticClass *) {}
void action_semantic_init(ActionSemantic *) {}
void value_semantic_class_init(ValueSemanticClass *) {}
void value_semantic_init(ValueSemantic *) {}

void extents(AtkComponent *object, gint *x, gint *y, gint *width, gint *height, AtkCoordType coordinates) {
  auto *w = node(object).widget();
  *x = *y = *width = *height = 0;
  if (!w) return;
  auto *window = w->top_window();
  if (!window || !window->shown()) return;
  // This extension is an X11 prototype. FLTK's top-level x()/y() may include
  // WM decorations; translate OUR native client origin, never a foreign window.
  const float scale = Fl::screen_scale(window->screen_num());
  *x = w == window ? 0 : static_cast<gint>(std::lround(w->x() * scale));
  *y = w == window ? 0 : static_cast<gint>(std::lround(w->y() * scale));
  *width = static_cast<gint>(std::lround(w->w() * scale));
  *height = static_cast<gint>(std::lround(w->h() * scale));
  if (coordinates == ATK_XY_SCREEN) {
    auto *display = fl_x11_display();
    const auto xid = fl_x11_xid(window);
    int origin_x{}, origin_y{};
    Window child{};
    if (!display || !xid || !XTranslateCoordinates(display, xid, DefaultRootWindow(display),
                                                   0, 0, &origin_x, &origin_y, &child)) {
      *x = *y = *width = *height = 0;
      return;
    }
    *x += origin_x; *y += origin_y;
  } else if (coordinates == ATK_XY_PARENT) {
    auto *parent = node(object).parent;
    auto *parent_widget = parent ? node(parent).widget() : nullptr;
    if (parent_widget && parent_widget != window) {
      *x -= static_cast<gint>(std::lround(parent_widget->x() * scale));
      *y -= static_cast<gint>(std::lround(parent_widget->y() * scale));
    }
  }
  // Synthetic rows/tab labels currently use their owner's bounds. Precise
  // row/header hit testing remains a documented prototype limitation.
}
gboolean grab_focus(AtkComponent *object) {
  auto &n = node(object);
  auto *w = n.widget();
  if (!w || !showing(n) || !w->active_r()) return FALSE;
  if (n.row) {
    auto *list = static_cast<Fl_Hold_Browser *>(w);
    list->value(n.row); list->do_callback();
  }
  const bool accepted = w->take_focus() != 0;
  w->redraw();
  return accepted;
}
void component_init(AtkComponentIface *type) {
  type->get_extents = extents;
  type->grab_focus = grab_focus;
  type->get_layer = [](AtkComponent *) { return ATK_LAYER_WIDGET; };
  type->get_alpha = [](AtkComponent *) { return 1.0; };
}

gboolean do_action(AtkAction *object, gint index) {
  auto &n = node(object);
  auto *w = n.widget();
  if (index != 0 || !w || !w->active_r() || !showing(n)) return FALSE;
  if (n.row) {
    auto *list = static_cast<Fl_Hold_Browser *>(w);
    list->value(n.row); list->do_callback(); list->redraw();
  } else if (auto *tabs = dynamic_cast<Fl_Tabs *>(w->parent())) {
    tabs->value(w); (void)w->take_focus(); tabs->redraw();
  } else {
    // Invoke the focused application's own command; never synthesize input.
    w->do_callback();
  }
  return TRUE;
}
void action_init(AtkActionIface *type) {
  type->get_n_actions = [](AtkAction *object) -> gint { return live(node(object)) ? 1 : 0; };
  type->do_action = do_action;
  type->get_name = [](AtkAction *, gint i) -> const gchar * { return i == 0 ? "activate" : nullptr; };
  type->get_description = [](AtkAction *, gint i) -> const gchar * { return i == 0 ? "Activate this control" : nullptr; };
  type->get_keybinding = [](AtkAction *, gint i) -> const gchar * { return i == 0 ? "" : nullptr; };
}

Fl_Input *input(void *object) { return dynamic_cast<Fl_Input *>(node(object).widget()); }
std::string input_text(void *object) { auto *w = input(object); return w ? w->value() : ""; }
gint char_count(const std::string &text) { return static_cast<gint>(g_utf8_strlen(text.c_str(), -1)); }
gint byte_offset(const std::string &text, gint offset) {
  offset = std::clamp(offset, 0, char_count(text));
  return static_cast<gint>(g_utf8_offset_to_pointer(text.c_str(), offset) - text.c_str());
}
gchar *text_range(AtkText *object, gint start, gint end) {
  const auto text = input_text(object);
  if (start < 0 || start > char_count(text) || (end != -1 && end < start)) return g_strdup("");
  const auto first = byte_offset(text, start);
  const auto last = end < 0 ? static_cast<gint>(text.size()) : byte_offset(text, end);
  return g_strndup(text.data() + first, static_cast<gsize>(last - first));
}
gint caret(AtkText *object) {
  auto *w = input(object);
  return w ? static_cast<gint>(g_utf8_strlen(w->value(), w->insert_position())) : 0;
}
gboolean set_caret(AtkText *object, gint offset) {
  auto *w = input(object);
  if (!w || offset < 0 || offset > char_count(w->value())) return FALSE;
  w->insert_position(byte_offset(w->value(), offset)); w->redraw(); return TRUE;
}
gchar *string_at(AtkText *object, gint offset, AtkTextGranularity granularity, gint *start, gint *end) {
  const auto text = input_text(object);
  const auto count = char_count(text);
  *start = *end = 0;
  if (offset < 0 || offset >= count) return g_strdup("");
  *start = offset; *end = offset + 1;
  if (granularity == ATK_TEXT_GRANULARITY_WORD) {
    const auto space = [&](gint i) { return g_unichar_isspace(g_utf8_get_char(text.c_str() + byte_offset(text, i))); };
    while (*start > 0 && !space(*start - 1)) --*start;
    while (*end < count && !space(*end)) ++*end;
  } else if (granularity != ATK_TEXT_GRANULARITY_CHAR) {
    *start = 0; *end = count; // These prototype fields are single-line.
  }
  return text_range(object, *start, *end);
}
void text_init(AtkTextIface *type) {
  type->get_text = text_range;
  type->get_character_count = [](AtkText *object) { return char_count(input_text(object)); };
  type->get_character_at_offset = [](AtkText *object, gint offset) -> gunichar {
    const auto text = input_text(object);
    return offset >= 0 && offset < char_count(text) ? g_utf8_get_char(text.c_str() + byte_offset(text, offset)) : 0;
  };
  type->get_caret_offset = caret;
  type->set_caret_offset = set_caret;
  type->get_string_at_offset = string_at;
  type->get_n_selections = [](AtkText *object) -> gint {
    auto *w = input(object); return w && w->insert_position() != w->mark() ? 1 : 0;
  };
  type->get_selection = [](AtkText *object, gint index, gint *start, gint *end) -> gchar * {
    auto *w = input(object);
    *start = *end = 0;
    if (!w || index != 0 || w->insert_position() == w->mark()) return nullptr;
    const auto a = std::min(w->insert_position(), w->mark());
    const auto b = std::max(w->insert_position(), w->mark());
    *start = static_cast<gint>(g_utf8_strlen(w->value(), a));
    *end = static_cast<gint>(g_utf8_strlen(w->value(), b));
    return text_range(object, *start, *end);
  };
  type->set_selection = [](AtkText *object, gint index, gint start, gint end) -> gboolean {
    auto *w = input(object);
    if (!w || index != 0 || start < 0 || end < start || end > char_count(w->value())) return FALSE;
    w->insert_position(byte_offset(w->value(), end), byte_offset(w->value(), start)); w->redraw(); return TRUE;
  };
}
void editable_init(AtkEditableTextIface *type) {
  type->set_text_contents = [](AtkEditableText *object, const gchar *text) {
    auto *w = input(object);
    if (!w || !w->active_r() || !showing(node(object)) || !text || !g_utf8_validate(text, -1, nullptr)) return;
    w->value(text); w->do_callback(); w->redraw();
  };
}
void numeric_value(GValue *value, double number) {
  g_value_init(value, G_TYPE_DOUBLE); g_value_set_double(value, number);
}
void value_init(AtkValueIface *type) {
  type->get_current_value = [](AtkValue *object, GValue *value) {
    auto *w = dynamic_cast<Fl_Spinner *>(node(object).widget()); numeric_value(value, w ? w->value() : 0.0);
  };
  type->get_minimum_value = [](AtkValue *object, GValue *value) {
    auto *w = dynamic_cast<Fl_Spinner *>(node(object).widget()); numeric_value(value, w ? w->minimum() : 0.0);
  };
  type->get_maximum_value = [](AtkValue *object, GValue *value) {
    auto *w = dynamic_cast<Fl_Spinner *>(node(object).widget()); numeric_value(value, w ? w->maximum() : 0.0);
  };
  type->get_minimum_increment = [](AtkValue *object, GValue *value) {
    auto *w = dynamic_cast<Fl_Spinner *>(node(object).widget()); numeric_value(value, w ? w->step() : 0.0);
  };
  type->set_current_value = [](AtkValue *object, const GValue *value) -> gboolean {
    auto *w = dynamic_cast<Fl_Spinner *>(node(object).widget());
    if (!w || !w->active_r() || !showing(node(object)) || !G_VALUE_HOLDS_DOUBLE(value)) return FALSE;
    const double number = g_value_get_double(value);
    if (!std::isfinite(number) || number < w->minimum() || number > w->maximum()) return FALSE;
    w->value(number); w->do_callback(); w->redraw(); return TRUE;
  };
}

AtkObject *root_object{};
AtkObject *get_root() { return root_object; }
const gchar *toolkit_name() { return "FLTK semantic prototype"; }
const gchar *toolkit_version() { return "1.4"; }
} // namespace

namespace layerc::prototype {
struct AccessibilityBridge::Impl {
  AtkObject *root{};
  std::vector<AtkObject *> objects;
  std::vector<GPollFD> descriptors;
  std::string fault_reason;
  bool initialized{};
  bool pumping{};
  GMainContext *context{g_main_context_default()};

  AtkObject *make(Fl_Widget *w, AtkObject *parent, int row = 0) {
    GType type = semantic_get_type();
    AtkRole role = ATK_ROLE_PANEL;
    if (!w) role = ATK_ROLE_APPLICATION;
    else if (row) { type = action_semantic_get_type(); role = ATK_ROLE_LIST_ITEM; }
    else if (dynamic_cast<Fl_Window *>(w)) role = ATK_ROLE_FRAME;
    else if (dynamic_cast<Fl_Input *>(w)) { type = input_semantic_get_type(); role = ATK_ROLE_ENTRY; }
    else if (dynamic_cast<Fl_Spinner *>(w)) { type = value_semantic_get_type(); role = ATK_ROLE_SPIN_BUTTON; }
    else if (dynamic_cast<Fl_Hold_Browser *>(w)) role = ATK_ROLE_LIST;
    else if (dynamic_cast<Fl_Tabs *>(w)) role = ATK_ROLE_PAGE_TAB_LIST;
    else if (dynamic_cast<Fl_Tabs *>(w->parent())) { type = action_semantic_get_type(); role = ATK_ROLE_PAGE_TAB; }
    else if (dynamic_cast<Fl_Button *>(w)) { type = action_semantic_get_type(); role = ATK_ROLE_PUSH_BUTTON; }
    else if (dynamic_cast<Fl_Box *>(w)) role = ATK_ROLE_LABEL;
    auto *object = ATK_OBJECT(g_object_new(type, nullptr));
    auto &n = node(object);
    n.parent = parent; n.row = row;
    if (w) n.tracker = std::make_unique<Fl_Widget_Tracker>(w);
    n.name = w ? clean_label(w->label()) : "layere_proto_fltk_a11y";
    if (n.name.empty() && role == ATK_ROLE_LIST) n.name = "Cooldowns";
    if (n.name.empty() && role == ATK_ROLE_PANEL && w && !dynamic_cast<Fl_Group *>(w)) n.name = "Panel preview";
    atk_object_set_role(object, role);
    atk_object_set_parent(object, parent);
    objects.push_back(object);
    if (parent) node(parent).children.push_back(object);
    if (auto *list = dynamic_cast<Fl_Hold_Browser *>(w); list && !row) {
      for (int i = 1; i <= list->size(); ++i) make(list, object, i);
    } else if (auto *group = dynamic_cast<Fl_Group *>(w); group && role != ATK_ROLE_SPIN_BUTTON) {
      for (int i = 0; i < group->children(); ++i) {
        auto *child = group->child(i);
        // The prototype's zero-sized Fl_Menu_Button is not the popup surface.
        if (child->w() > 0 && child->h() > 0) make(child, object);
      }
    }
    n.focused = focused(n); n.selected = selected(n); n.text = input_text(object);
    n.last_name = get_name(object);
    return object;
  }

  explicit Impl(Fl_Window &window) {
    root = make(nullptr, nullptr);
    make(&window, root);
    root_object = root;
    auto *util = static_cast<AtkUtilClass *>(g_type_class_ref(ATK_TYPE_UTIL));
    util->get_root = get_root;
    util->get_toolkit_name = toolkit_name;
    util->get_toolkit_version = toolkit_version;
    g_type_class_unref(util);
    const auto *disabled = g_getenv("NO_AT_BRIDGE");
    const bool disabled_by_user = disabled && std::string(disabled) == "1";
    if (!disabled_by_user) {
      if (auto closure = bridge_closure_fault(); !closure.empty()) {
        fault_reason = std::move(closure);
        return;
      }
    }
    initialized = !disabled_by_user && atk_bridge_adaptor_init(nullptr, nullptr) == 0;
    if (!initialized) {
      fault_reason = "AT-SPI bus unavailable or bridge disabled";
      return;
    }
    atk_bridge_set_event_context(context);
    Fl::add_check(check, this);
    pump();
  }
  ~Impl() {
    Fl::remove_check(check, this);
    Fl::remove_timeout(wake, this);
    remove_descriptors();
    // Keep accessibles alive while the adaptor retires its leases and weak
    // references. Detach the tree first so tidy_windows() sees an empty root.
    for (auto *object : objects) {
      node(object).children.clear();
      node(object).parent = nullptr;
      atk_object_set_parent(object, nullptr);
    }
    if (initialized) atk_bridge_adaptor_cleanup();
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) g_object_unref(*it);
    objects.clear();
    root_object = nullptr;
    root = nullptr;
  }
  void remove_descriptors() {
    for (const auto &fd : descriptors) Fl::remove_fd(fd.fd, FL_READ | FL_WRITE | FL_EXCEPT);
    descriptors.clear();
  }
  static void ready(int, void *self) { static_cast<Impl *>(self)->pump(); }
  static void wake(void *self) { static_cast<Impl *>(self)->pump(); }
  static void check(void *self) { static_cast<Impl *>(self)->pump(); }
  void sync() {
    for (auto *object : objects) {
      auto &n = node(object);
      const std::string next_name = get_name(object);
      if (next_name != n.last_name) {
        n.last_name = next_name;
        g_object_notify(G_OBJECT(object), "accessible-name");
      }
      const bool next_focus = focused(n), next_selected = selected(n);
      if (next_focus != n.focused) {
        n.focused = next_focus;
        atk_object_notify_state_change(object, ATK_STATE_FOCUSED, next_focus);
      }
      if (next_selected != n.selected) {
        n.selected = next_selected;
        atk_object_notify_state_change(object, ATK_STATE_SELECTED, next_selected);
      }
      if (ATK_IS_TEXT(object)) {
        const auto text = input_text(object);
        if (text != n.text) {
          const auto old_count = char_count(n.text);
          n.text = text;
          g_signal_emit_by_name(object, "text-changed::delete", 0, old_count);
          g_signal_emit_by_name(object, "text-changed::insert", 0, char_count(text));
        }
      }
    }
  }
  void pump() {
    if (pumping || !initialized || !g_main_context_acquire(context)) return;
    pumping = true;
    sync();
    // Integrate GLib's readiness descriptors/deadlines into FLTK's blocking
    // loop. No fixed-rate timer, busy idle callback, or auxiliary UI thread.
    for (unsigned i = 0; i < 64 && g_main_context_pending(context); ++i)
      g_main_context_iteration(context, FALSE);
    sync();
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

AccessibilityBridge::AccessibilityBridge(Fl_Window &window) : impl_(std::make_unique<Impl>(window)) {}
AccessibilityBridge::~AccessibilityBridge() = default;
bool AccessibilityBridge::available() const { return impl_->initialized; }
const std::string &AccessibilityBridge::fault() const { return impl_->fault_reason; }
} // namespace layerc::prototype
