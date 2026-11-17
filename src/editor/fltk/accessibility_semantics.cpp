#include "accessibility_semantics.hpp"
#include "a11y/window_geometry.hpp"
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

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace layerc::editor::accessibility {

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

Key target(const Node &n) { return {n.widget(), n.kind, n.index}; }
const Fl_Menu_Item *menu_item(const Node &n) {
  return live(n) ? a11y::menu_item_of(target(n)) : nullptr;
}
bool selected(const Node &n) { return live(n) && a11y::selected(target(n)); }
bool focused(const Node &n) { return live(n) && a11y::focused(target(n)); }
bool showing(const Node &n) { return live(n) && a11y::showing(target(n)); }
bool enabled(const Node &n) { return live(n) && a11y::enabled(target(n)); }
bool checked(const Node &n) { return live(n) && a11y::checked(target(n)); }
using a11y::modal_allows;
Fl_Input_ *input(void *object) {
  auto &n = node(object);
  return n.kind == Kind::widget && live(n) ? dynamic_cast<Fl_Input_ *>(n.widget()) : nullptr;
}
std::string valid_utf8(const std::string &text) { return a11y::valid_utf8(text); }
std::string input_text(void *object) {
  const auto &n = node(object);
  return live(n) ? a11y::input_text(target(n)) : std::string{};
}
std::string current_keybinding(const Node &n) {
  return live(n) ? a11y::keybinding(target(n)) : std::string{};
}
std::string current_name(const Node &n) {
  return live(n) ? a11y::name(target(n)) : std::string{};
}
std::string current_description(AtkObject *object) {
  const auto &n = node(object);
  if (!live(n)) return {};
  const auto inherited = ATK_OBJECT_CLASS(semantic_parent_class)->get_description;
  const auto *text = inherited != nullptr ? inherited(object) : nullptr;
  return a11y::description(target(n), text != nullptr ? text : "");
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
  const auto flags = a11y::states(target(n));
  const auto add = [&](bool present, AtkStateType type) {
    if (present) atk_state_set_add_state(state, type);
  };
  add(flags.visible, ATK_STATE_VISIBLE);
  add(flags.showing, ATK_STATE_SHOWING);
  add(!flags.disabled, ATK_STATE_ENABLED);
  add(!flags.disabled, ATK_STATE_SENSITIVE);
  add(flags.modal, ATK_STATE_MODAL);
  add(flags.active, ATK_STATE_ACTIVE);
  add(flags.resizable, ATK_STATE_RESIZABLE);
  add(flags.focusable, ATK_STATE_FOCUSABLE);
  add(flags.editable, ATK_STATE_EDITABLE);
  if (n.kind == Kind::widget && dynamic_cast<Fl_Input_ *>(n.widget()))
    add(true, flags.multi_line ? ATK_STATE_MULTI_LINE : ATK_STATE_SINGLE_LINE);
  add(flags.checkable, ATK_STATE_CHECKABLE);
  add(flags.selectable, ATK_STATE_SELECTABLE);
  add(flags.checked, ATK_STATE_CHECKED);
  add(flags.selected, ATK_STATE_SELECTED);
  add(flags.focused, ATK_STATE_FOCUSED);
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
  const auto bounds = a11y::local_bounds(target(n));
  if (!bounds) return;
  const auto left = bounds->left, top_y = bounds->top;
  const auto width_units = bounds->width, height_units = bounds->height;
  const float scale = Fl::screen_scale(window->screen_num());
  const bool top = w == window && n.kind != Kind::menu_item;
  *x = top ? 0 : static_cast<gint>(std::lround(left * scale));
  *y = top ? 0 : static_cast<gint>(std::lround(top_y * scale));
  *width = static_cast<gint>(std::lround(width_units * scale));
  *height = static_cast<gint>(std::lround(height_units * scale));
  if (coordinates == ATK_XY_SCREEN) {
    const auto origin = a11y::window_screen_origin(*window);
    if (!origin) {
      *x = *y = *width = *height = 0;
      return;
    }
    *x += origin->first;
    *y += origin->second;
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
  return w && a11y::editable(target(node(object))) ? w : nullptr;
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
  using a11y::Role;
  switch (a11y::role_of(key)) {
  case Role::application: role = ATK_ROLE_APPLICATION; break;
  case Role::window: role = ATK_ROLE_FRAME; type = window_semantic_get_type(); break;
  case Role::dialog: role = ATK_ROLE_DIALOG; type = window_semantic_get_type(); break;
  case Role::text_field:
    role = a11y::states(key).read_only ? ATK_ROLE_TEXT : ATK_ROLE_ENTRY;
    type = text_semantic_get_type();
    break;
  case Role::number_field: role = ATK_ROLE_SPIN_BUTTON; type = value_semantic_get_type(); break;
  case Role::scroll_bar: role = ATK_ROLE_SCROLL_BAR; type = value_semantic_get_type(); break;
  case Role::slider: role = ATK_ROLE_SLIDER; type = value_semantic_get_type(); break;
  case Role::menu_bar: role = ATK_ROLE_MENU_BAR; break;
  case Role::menu:
    role = ATK_ROLE_MENU;
    type = key.kind == Kind::menu_item ? action_semantic_get_type() : selection_semantic_get_type();
    break;
  case Role::drop_down: role = ATK_ROLE_COMBO_BOX; type = selection_semantic_get_type(); break;
  case Role::list: role = ATK_ROLE_LIST; type = selection_semantic_get_type(); break;
  case Role::tab_list: role = ATK_ROLE_PAGE_TAB_LIST; type = selection_semantic_get_type(); break;
  case Role::tab: role = ATK_ROLE_PAGE_TAB; type = action_semantic_get_type(); break;
  case Role::list_row: role = ATK_ROLE_LIST_ITEM; type = action_semantic_get_type(); break;
  case Role::menu_item: role = ATK_ROLE_MENU_ITEM; type = action_semantic_get_type(); break;
  case Role::check_menu_item: role = ATK_ROLE_CHECK_MENU_ITEM; type = action_semantic_get_type(); break;
  case Role::radio_menu_item: role = ATK_ROLE_RADIO_MENU_ITEM; type = action_semantic_get_type(); break;
  case Role::check_box: role = ATK_ROLE_CHECK_BOX; type = action_semantic_get_type(); break;
  case Role::radio_button: role = ATK_ROLE_RADIO_BUTTON; type = action_semantic_get_type(); break;
  case Role::button: role = ATK_ROLE_PUSH_BUTTON; type = action_semantic_get_type(); break;
  case Role::label: role = ATK_ROLE_LABEL; break;
  case Role::status: role = ATK_ROLE_STATUSBAR; break;
  case Role::image: role = ATK_ROLE_IMAGE; break;
  case Role::scroll_pane: role = ATK_ROLE_SCROLL_PANE; break;
  case Role::canvas: role = ATK_ROLE_CANVAS; break;
  case Role::group: break;
  }
  auto *object = ATK_OBJECT(g_object_new(type, nullptr));
  auto &n = node(object);
  n.kind = key.kind;
  n.index = key.index;
  n.parent = parent;
  if (w) n.tracker = std::make_unique<Fl_Widget_Tracker>(w);
  atk_object_set_role(object, role);
  atk_object_set_parent(object, parent);
  remember(object);
  return object;
}
std::vector<Key> desired(AtkObject *object) {
  const auto &n = node(object);
  if (!live(n)) return {};
  std::vector<Key> previous;
  previous.reserve(n.children.size());
  for (auto *child : n.children) previous.push_back(target(node(child)));
  return a11y::desired_children(target(n), previous);
}

} // namespace layerc::editor::accessibility
