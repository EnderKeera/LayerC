// Regressions for review/archiv/layer2.review-2026-11-15.md. Native widgets in a
// disposable X11 display; these checks are not physical/Orca speech evidence.
#include "accessibility_tree.hpp"
#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_commands.hpp"
#include "editor_popup.hpp"
#include "editor_scroll.hpp"
#include "editor_style.hpp"
#include "test_support.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <X11/Xutil.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string_view>

namespace {
namespace editor = layerc::editor;
namespace a11y = editor::accessibility;
using layerc::test::expect;
AtkObject *find(AtkObject *root, Fl_Widget &widget, a11y::Kind kind = a11y::Kind::widget, int index = 0) {
  const auto &node = a11y::node(root);
  if (node.widget() == &widget && node.kind == kind && node.index == index) return root;
  for (auto *child : node.children) if (auto *found = find(child, widget, kind, index)) return found;
  return nullptr;
}
bool state(AtkObject *object, AtkStateType type) {
  auto *set = atk_object_ref_state_set(object);
  const bool answer = atk_state_set_contains_state(set, type) != 0;
  g_object_unref(set); return answer;
}
std::array<gint, 4> bounds(AtkObject *object) {
  std::array<gint, 4> result{};
  atk_component_get_extents(ATK_COMPONENT(object), &result[0], &result[1], &result[2], &result[3], ATK_XY_WINDOW);
  return result;
}
template <class T> T *control(Fl_Group &group, std::string_view name) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *typed = dynamic_cast<T *>(child); typed && child->visible_r() &&
        a11y::accessible_name(*child).value_or(child->label() ? child->label() : "") == name) return typed;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *found = control<T>(*nested, name)) return found;
  }
  return nullptr;
}
class DuringModal final {
public:
  explicit DuringModal(std::function<void(Fl_Window &)> callback) : callback_(std::move(callback)) {
    Fl::add_timeout(.01, run, this);
  }
  ~DuringModal() { Fl::remove_timeout(run, this); expect(delivered_, "modal regression probe ran"); }
private:
  std::function<void(Fl_Window &)> callback_;
  int attempts_{};
  bool delivered_{};
  static void run(void *data) {
    auto &self = *static_cast<DuringModal *>(data);
    if (auto *modal = Fl::modal()) { self.delivered_ = true; self.callback_(*modal); }
    else if (++self.attempts_ < 100) Fl::repeat_timeout(.01, run, data);
    else expect(false, "modal regression probe timed out");
  }
};
void key(int value) {
  const int previous = Fl::e_keysym;
  Fl::e_keysym = value;
  if (auto *grab = Fl::grab()) grab->handle(FL_KEYBOARD);
  Fl::e_keysym = previous;
}
void scroll_focus_and_effective_extents() {
  Fl_Window window(500, 300);
  editor::EditorScroll host(0, 0, 500, 300);
  Fl_Group canvas(0, 0, 900, 700);
  Fl_Input near(12, 12, 160, 28, "Near");
  Fl_Input far(680, 600, 160, 28, "Far");
  canvas.end(); host.end(); window.resizable(host); window.end();
  window.show(); near.take_focus(); Fl::check();
  a11y::AccessibilityTree tree; std::string fault;
  if (!expect(tree.initialize(fault), "LNX-01 tree initializes")) return;
  tree.sync();
  auto *object = find(atk_get_root(), far);
  if (!expect(object != nullptr, "LNX-01 offscreen widget stays exported")) return;
  expect(!state(object, ATK_STATE_SHOWING) && bounds(object) == std::array<gint, 4>{},
         "LNX-01 fully clipped controls are not showing and have zero extents");
  expect(far.take_focus() != 0, "LNX-01 offscreen input can take keyboard focus");
  Fl::check(); tree.sync();
  const auto view = host.viewport();
  expect(host.xposition() > 0 && host.yposition() > 0 && far.x() >= view.x && far.y() >= view.y &&
             far.x() + far.w() <= view.x + view.width && far.y() + far.h() <= view.y + view.height,
         "LNX-01 a focus transition reveals the whole input on both axes, excluding scrollbars");
  expect(state(object, ATK_STATE_SHOWING) && state(object, ATK_STATE_FOCUSED),
         "LNX-01 revealed input reports focused and showing");
  host.scroll_to(host.xposition() + far.x() - host.viewport().x + far.w() / 2, host.yposition());
  Fl::check(); tree.sync();
  const auto partial = bounds(object);
  expect(partial[2] > 0 && partial[2] < far.w() && state(object, ATK_STATE_SHOWING),
         "LNX-01 partially clipped extents are intersected, without undoing manual scroll");
  near.take_focus(); Fl::check(); tree.sync();
  expect(!state(object, ATK_STATE_SHOWING), "LNX-01 reverse focus scrolls back and clips the distant input");
  window.hide();
}
void popup_semantics_and_recovery() {
  Fl_Window window(460, 300);
  editor::EditorMenuBar bar(0, 0, 460, 28);
  int invoked = 0;
  auto callback = [](Fl_Widget *, void *data) { ++*static_cast<int *>(data); };
  bar.add("&Help/&First", 0, callback, &invoked);
  bar.add("&Help/Disabled", 0, callback, &invoked, FL_MENU_INACTIVE);
  bar.add("&Help/&Last", 0, callback, &invoked);
  bar.add("&View/&Size/&Small", 0, callback, &invoked);
  bar.add("&View/&Size/&Large", 0, callback, &invoked);
  editor::EditorChoice choice(20, 80, 200, editor::editor_input_height, "Mode");
  choice.add("One|Two|Three"); choice.value(0); choice.callback(callback, &invoked);
  Fl_Button previous(20, 150, 150, editor::editor_button_height, "Previous focus");
  window.end(); window.show(); previous.take_focus(); Fl::check();
  a11y::AccessibilityTree tree; std::string fault;
  if (!expect(tree.initialize(fault), "LNX-02 tree initializes")) return;
  tree.sync();
  auto *first = find(atk_get_root(), bar, a11y::Kind::menu_item, 1);
  auto *last = find(atk_get_root(), bar, a11y::Kind::menu_item, 3);
  auto *previous_object = find(atk_get_root(), previous);
  if (!expect(first && last && previous_object, "LNX-02 menu items and focus owner are exported")) return;
  expect(!state(first, ATK_STATE_SHOWING) && bounds(first) == std::array<gint, 4>{},
         "LNX-02 closed dropdown items have no onscreen rectangle");
  int focus_events = 0;
  g_signal_connect(first, "state-change", G_CALLBACK(+[](AtkObject *, const gchar *name, gboolean value, void *data) {
    if (std::string_view(name) == "focused" && value) ++*static_cast<int *>(data);
  }), &focus_events);
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); tree.sync();
      expect(state(first, ATK_STATE_ENABLED) && state(first, ATK_STATE_SHOWING) &&
                 state(first, ATK_STATE_FOCUSED) && state(first, ATK_STATE_SELECTED),
             "LNX-02 highlighted open item is enabled/showing/focused/selected");
      expect(state(previous_object, ATK_STATE_ENABLED) && !state(previous_object, ATK_STATE_FOCUSED),
             "LNX-02 popup preserves real owner enabled state but owns focus");
      const auto one = bounds(first), other = bounds(last);
      expect(one[2] > 0 && one[3] > 0 && one[1] + one[3] <= other[1],
             "LNX-02 visible menu rows have distinct ordered extents");
      key(FL_Down); tree.sync();
      expect(state(last, ATK_STATE_FOCUSED) && !state(first, ATK_STATE_FOCUSED) && focus_events == 1,
             "LNX-02 arrows skip disabled items and emit focused state changes");
      key(FL_Escape);
    });
    editor::track_editor_popup(bar, 0);
  }
  tree.sync();
  expect(!editor::popup_owner() && !Fl::grab() && invoked == 0 &&
             state(previous_object, ATK_STATE_FOCUSED) && !state(last, ATK_STATE_SHOWING),
         "LNX-02 Escape releases tracking, restores focus and invokes nothing");
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); tree.sync();
      auto *one = find(atk_get_root(), choice, a11y::Kind::menu_item, 0);
      auto *two = find(atk_get_root(), choice, a11y::Kind::menu_item, 1);
      expect(one && two && choice.value() == 0 && !state(one, ATK_STATE_SELECTED) &&
                 state(two, ATK_STATE_SELECTED) && state(two, ATK_STATE_FOCUSED),
             "LNX-02 open choice selection follows highlight, not the committed value");
      key(FL_Enter);
    });
    editor::track_editor_popup(choice);
  }
  expect(choice.value() == 1 && invoked == 1, "LNX-02 choice commits once after tracking ends");
  {
    DuringModal probe([&](Fl_Window &) { key(FL_Down); key(FL_Escape); });
    editor::track_editor_popup(choice);
  }
  expect(choice.value() == 1 && invoked == 1, "LNX-02 cancelled choice retains the committed value");
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); key(FL_Right); tree.sync();
      const int small = static_cast<int>(bar.find_item("&View/&Size/&Small") - bar.menu());
      auto *item = find(atk_get_root(), bar, a11y::Kind::menu_item, small);
      expect(item && state(item, ATK_STATE_FOCUSED) && state(item, ATK_STATE_SHOWING),
             "LNX-02 keyboard reaches an owned nested submenu");
      if (item) expect(atk_action_do_action(ATK_ACTION(item), 0) != 0,
                       "LNX-02 accessible popup action queues a pick");
      else key(FL_Escape);
    });
    const int view = static_cast<int>(bar.find_item("&View") - bar.menu());
    editor::track_editor_popup(bar, view);
  }
  expect(invoked == 2 && !editor::popup_owner() && !Fl::grab(),
         "LNX-02 deferred accessible pick closes the modal wait and invokes once");
  Fl_Group::current(&window);
  Fl_Menu_Button context(0, 0, 0, 0); context.add("Action", 0, callback, &invoked);
  Fl_Group::current(nullptr);
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); tree.sync();
      auto *owner = find(atk_get_root(), context);
      auto *item = find(atk_get_root(), context, a11y::Kind::menu_item, 0);
      expect(owner && item && state(item, ATK_STATE_ENABLED) && state(item, ATK_STATE_FOCUSED),
             "LNX-02 zero-sized temporary context owner is exported while tracking");
      key(FL_Escape);
    });
    editor::track_editor_popup(context);
  }
  tree.sync();
  expect(!find(atk_get_root(), context), "LNX-02 closed temporary context owner is removed");
  g_signal_handlers_disconnect_by_data(first, &focus_events);
  window.hide();
}
void text_dialog_layouts() {
  const auto inspect = [](Fl_Window &modal) {
    auto *close = control<Fl_Button>(modal, "Close");
    if (!expect(close != nullptr, "LNX-03 dialog has a Close action")) { modal.hide(); return; }
    const int width = close->w(), height = close->h();
    const int right = modal.w() - close->x() - width, bottom = modal.h() - close->y() - height;
    XSizeHints hints{}; long supplied{};
    expect(XGetWMNormalHints(fl_display, fl_xid(&modal), &hints, &supplied) && (hints.flags & PMinSize) &&
               hints.min_width > width && hints.min_height > height,
           "LNX-03 measured minimum geometry is published to the window manager");
    for (const auto size : {std::array{1400, 320}, std::array{static_cast<int>(hints.min_width), 850}, std::array{560, 400}}) {
      modal.size(std::max(size[0], static_cast<int>(hints.min_width)),
                   std::max(size[1], static_cast<int>(hints.min_height)));
      expect(close->w() == width && close->h() == height && modal.w() - close->x() - close->w() == right &&
                 modal.h() - close->y() - close->h() == bottom,
             "LNX-03 resizing preserves fixed action dimensions and right/bottom anchors");
      if (auto *copy = control<Fl_Button>(modal, "&Copy details"))
        expect(copy->x() >= 0 && copy->x() + copy->w() < close->x() && copy->w() >= 110,
               "LNX-03 About copy action stays fixed and does not overlap Close");
    }
    close->do_callback();
  };
  { DuringModal probe(inspect); editor::show_shortcuts_dialog(editor::shortcut_help()); }
  { DuringModal probe(inspect); editor::show_text_dialog("Options — LayerE", "Options content"); }
  { DuringModal probe(inspect); editor::show_text_dialog("About LayerE", "Version details", "Version details"); }
}
void named_color_chooser_and_roles() {
  Fl_Window owner(400, 250);
  Fl_Value_Input number(10, 10, 100, 28, "Number");
  Fl_Slider slider(10, 50, 200, 28, "Slider");
  Fl_Scrollbar scrollbar(10, 90, 200, 28, "Scrollbar");
  owner.end(); owner.show(); Fl::check();
  a11y::AccessibilityTree tree; std::string fault;
  if (!expect(tree.initialize(fault), "LNX-04 tree initializes")) return;
  tree.sync();
  expect(atk_object_get_role(find(atk_get_root(), number)) == ATK_ROLE_SPIN_BUTTON &&
             atk_object_get_role(find(atk_get_root(), slider)) == ATK_ROLE_SLIDER &&
             atk_object_get_role(find(atk_get_root(), scrollbar)) == ATK_ROLE_SCROLL_BAR,
         "LNX-04 concrete valuators have spin/slider/scrollbar roles");
  {
    DuringModal probe([&](Fl_Window &modal) {
      tree.sync();
      auto *mode = control<editor::EditorChoice>(modal, "Color mode");
      auto *red = control<Fl_Value_Input>(modal, "Red");
      auto *green = control<Fl_Value_Input>(modal, "Green");
      auto *blue = control<Fl_Value_Input>(modal, "Blue");
      expect(mode && red && green && blue && control<Fl_Widget>(modal, "Hue and saturation") &&
                 control<Fl_Widget>(modal, "Value"), "LNX-04 native chooser controls have semantic names");
      if (mode && red && green && blue) {
        for (auto *channel : {red, green, blue})
          expect(atk_object_get_role(find(atk_get_root(), *channel)) == ATK_ROLE_SPIN_BUTTON,
                 "LNX-04 chooser channels are spin buttons");
        mode->value(3); mode->do_callback(); tree.sync();
        expect(control<Fl_Value_Input>(modal, "Hue") == red && control<Fl_Value_Input>(modal, "Saturation") == green &&
                   control<Fl_Value_Input>(modal, "Value") == blue,
               "LNX-04 HSV mode renames the same native channel controls");
      }
      control<Fl_Button>(modal, "Cancel")->do_callback();
    });
    expect(!editor::choose_color("Fill", 0x50BAC8), "LNX-04 cancel does not return a changed color");
  }
  {
    DuringModal probe([](Fl_Window &modal) { control<Fl_Button>(modal, "OK")->do_callback(); });
    expect(editor::choose_color("Fill", 0x50BAC8) == 0x50BAC8, "LNX-04 accepting preserves the initial RGB value");
  }
  owner.hide();
}
} // namespace
void linux_editor_review_regressions() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  scroll_focus_and_effective_extents();
  popup_semantics_and_recovery();
  text_dialog_layouts();
  named_color_chooser_and_roles();
}
