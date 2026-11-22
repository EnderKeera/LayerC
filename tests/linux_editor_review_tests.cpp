// Regressions for review/archiv/layer2.review-2026-11-15.md. Native widgets in a
// disposable X11 display; these checks are not physical/Orca speech evidence.
#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_commands.hpp"
#include "editor_popup.hpp"
#include "editor_scroll.hpp"
#include "editor_style.hpp"
#include "fltk_a11y_snapshot_support.hpp"
#include "editor_test_environment.hpp"
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
#ifndef _WIN32
#include <X11/Xutil.h>
#endif
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <string_view>

namespace {
namespace editor = layerc::editor;
namespace a11y = layerc::editor::a11y;
namespace a11y_snapshot = layerc::test::a11y_snapshot;
using layerc::test::expect;
// The node owning `widget`, or the row or menu item it publishes at `index`.
const a11y::Node *find(a11y::FltkTree &tree, const a11y::Snapshot &snapshot,
                       Fl_Widget &widget,
                       const a11y::TargetKind kind = a11y::TargetKind::widget,
                       const int index = 0) {
  return a11y_snapshot::find(tree, snapshot, widget, kind, index);
}
// The focused node in a pass, so a test can tell whether focus moved between
// two passes the way the legacy state-change signal reported.
const a11y::Node *focused_node(const a11y::Snapshot &snapshot) {
  for (const auto &[id, node] : snapshot.nodes) {
    (void)id;
    if (node.states.focused) return &node;
  }
  return nullptr;
}
template <class T> T *control(Fl_Group &group, std::string_view name) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *typed = dynamic_cast<T *>(child); typed && child->visible_r() &&
        editor::accessibility::accessible_name(*child).value_or(child->label() ? child->label() : "") == name) return typed;
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
  Fl_Input near_field(12, 12, 160, 28, "Near");
  Fl_Input far_field(680, 600, 160, 28, "Far");
  canvas.end(); host.end(); window.resizable(host); window.end();
  window.show(); near_field.take_focus(); Fl::check();
  a11y::FltkTree tree;
  auto snapshot = tree.snapshot();
  const a11y::Node *object = find(tree, snapshot, far_field);
  if (!expect(object != nullptr, "LNX-01 offscreen widget stays exported")) return;
  const auto far_id = object->id;
  expect(!a11y_snapshot::showing(object) &&
             a11y_snapshot::bounds(object) == a11y_snapshot::Bounds{},
         "LNX-01 fully clipped controls are not showing and have zero extents");
  expect(far_field.take_focus() != 0, "LNX-01 offscreen input can take keyboard focus");
  Fl::check();
  snapshot = tree.snapshot();
  object = a11y_snapshot::lookup(snapshot, far_id);
  const auto view = host.viewport();
  expect(host.xposition() > 0 && host.yposition() > 0 && far_field.x() >= view.x && far_field.y() >= view.y &&
             far_field.x() + far_field.w() <= view.x + view.width && far_field.y() + far_field.h() <= view.y + view.height,
         "LNX-01 a focus transition reveals the whole input on both axes, excluding scrollbars");
  expect(a11y_snapshot::showing(object) && a11y_snapshot::focused(object),
         "LNX-01 revealed input reports focused and showing");
  host.scroll_to(host.xposition() + far_field.x() - host.viewport().x + far_field.w() / 2, host.yposition());
  Fl::check();
  snapshot = tree.snapshot();
  object = a11y_snapshot::lookup(snapshot, far_id);
  const auto partial = a11y_snapshot::bounds(object);
  expect(partial[2] > 0 && partial[2] < far_field.w() && a11y_snapshot::showing(object),
         "LNX-01 partially clipped extents are intersected, without undoing manual scroll");
  near_field.take_focus();
  Fl::check();
  snapshot = tree.snapshot();
  object = a11y_snapshot::lookup(snapshot, far_id);
  expect(!a11y_snapshot::showing(object),
         "LNX-01 reverse focus scrolls back and clips the distant input");
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
  a11y::FltkTree tree;
  auto snapshot = tree.snapshot();
  const a11y::Node *first = find(tree, snapshot, bar, a11y::TargetKind::menu_item, 1);
  const a11y::Node *last = find(tree, snapshot, bar, a11y::TargetKind::menu_item, 3);
  const a11y::Node *previous_object = find(tree, snapshot, previous);
  if (!expect(first && last && previous_object, "LNX-02 menu items and focus owner are exported")) return;
  const auto first_id = first->id;
  const auto last_id = last->id;
  const auto previous_id = previous_object->id;
  expect(!a11y_snapshot::showing(first) &&
             a11y_snapshot::bounds(first) == a11y_snapshot::Bounds{},
         "LNX-02 closed dropdown items have no onscreen rectangle");
  // Focus entering the first row, counted between snapshot passes. This sees
  // the model only; delivery of the focused-state event over the bus is
  // asserted in linux_editor_accessibility_popup_focus_events.
  int focus_events = 0;
  a11y::NodeId observed_focus = focused_node(snapshot)
                                    ? focused_node(snapshot)->id
                                    : a11y::no_node;
  const auto sync = [&] {
    snapshot = tree.snapshot();
    const auto now = focused_node(snapshot);
    const auto focused_id = now ? now->id : a11y::no_node;
    if (focused_id == first_id && focused_id != observed_focus) ++focus_events;
    observed_focus = focused_id;
    first = a11y_snapshot::lookup(snapshot, first_id);
    last = a11y_snapshot::lookup(snapshot, last_id);
    previous_object = a11y_snapshot::lookup(snapshot, previous_id);
  };
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); sync();
      expect(a11y_snapshot::enabled(first) && a11y_snapshot::showing(first) &&
                 a11y_snapshot::focused(first) && a11y_snapshot::selected(first),
             "LNX-02 highlighted open item is enabled/showing/focused/selected");
      expect(a11y_snapshot::enabled(previous_object) && !a11y_snapshot::focused(previous_object),
             "LNX-02 popup preserves real owner enabled state but owns focus");
      const auto one = a11y_snapshot::bounds(first), other = a11y_snapshot::bounds(last);
      expect(one[2] > 0 && one[3] > 0 && one[1] + one[3] <= other[1],
             "LNX-02 visible menu rows have distinct ordered extents");
      key(FL_Down); sync();
      expect(a11y_snapshot::focused(last) && !a11y_snapshot::focused(first) && focus_events == 1,
             "LNX-02 arrows skip disabled items and move focus into the first row once");
      key(FL_Escape);
    });
    editor::track_editor_popup(bar, 0);
  }
  sync();
  expect(!editor::popup_owner() && !Fl::grab() && invoked == 0 &&
             a11y_snapshot::focused(previous_object) && !a11y_snapshot::showing(last),
         "LNX-02 Escape releases tracking, restores focus and invokes nothing");
  {
    DuringModal probe([&](Fl_Window &) {
      key(FL_Down); sync();
      const auto *one =
          find(tree, snapshot, choice, a11y::TargetKind::menu_item, 0);
      const auto *two =
          find(tree, snapshot, choice, a11y::TargetKind::menu_item, 1);
      expect(one && two && choice.value() == 0 && !a11y_snapshot::selected(one) &&
                 a11y_snapshot::selected(two) && a11y_snapshot::focused(two),
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
      key(FL_Down); key(FL_Right); sync();
      const int small = static_cast<int>(bar.find_item("&View/&Size/&Small") - bar.menu());
      const auto *item = find(tree, snapshot, bar, a11y::TargetKind::menu_item, small);
      expect(item && a11y_snapshot::focused(item) && a11y_snapshot::showing(item),
             "LNX-02 keyboard reaches an owned nested submenu");
      if (item) expect(a11y_snapshot::activate(tree, item),
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
      key(FL_Down); sync();
      const auto *owner = find(tree, snapshot, context);
      const auto *item = find(tree, snapshot, context, a11y::TargetKind::menu_item, 0);
      expect(owner && item && a11y_snapshot::enabled(item) && a11y_snapshot::focused(item),
             "LNX-02 zero-sized temporary context owner is exported while tracking");
      key(FL_Escape);
    });
    editor::track_editor_popup(context);
  }
  sync();
  expect(find(tree, snapshot, context) == nullptr,
         "LNX-02 closed temporary context owner is removed");
  window.hide();
}
void text_dialog_layouts() {
  const auto inspect = [](Fl_Window &modal) {
    auto *close = control<Fl_Button>(modal, "Close");
    if (!expect(close != nullptr, "LNX-03 dialog has a Close action")) { modal.hide(); return; }
    const int width = close->w(), height = close->h();
    const int right = modal.w() - close->x() - width, bottom = modal.h() - close->y() - height;
#ifdef _WIN32
    int min_width{}, min_height{};
    modal.get_size_range(&min_width, &min_height, nullptr, nullptr, nullptr, nullptr, nullptr);
    MINMAXINFO info{};
    SendMessageW(fl_xid(&modal), WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info));
    const float scale = Fl::screen_scale(modal.screen_num());
    expect(min_width > width && min_height > height &&
               info.ptMinTrackSize.x >= static_cast<LONG>(scale * static_cast<float>(min_width)) &&
               info.ptMinTrackSize.y >= static_cast<LONG>(scale * static_cast<float>(min_height)),
           "measured minimum geometry is published in Windows track sizes");
    const std::array sizes{std::array{1400, 320}, std::array{min_width, 850}, std::array{560, 400}};
    for (const auto size : sizes) {
      modal.size(std::max(size[0], min_width), std::max(size[1], min_height));
#else
    XSizeHints hints{}; long supplied{};
    expect(XGetWMNormalHints(fl_display, fl_xid(&modal), &hints, &supplied) && (hints.flags & PMinSize) &&
               hints.min_width > width && hints.min_height > height,
           "LNX-03 measured minimum geometry is published to the window manager");
    for (const auto size : {std::array{1400, 320}, std::array{static_cast<int>(hints.min_width), 850}, std::array{560, 400}}) {
      modal.size(std::max(size[0], static_cast<int>(hints.min_width)),
                   std::max(size[1], static_cast<int>(hints.min_height)));
#endif
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
  a11y::FltkTree tree;
  auto snapshot = tree.snapshot();
  const auto *number_node = find(tree, snapshot, number);
  const auto *slider_node = find(tree, snapshot, slider);
  const auto *scrollbar_node = find(tree, snapshot, scrollbar);
  expect(number_node && slider_node && scrollbar_node &&
             number_node->role == a11y::Role::number_field &&
             slider_node->role == a11y::Role::slider &&
             scrollbar_node->role == a11y::Role::scroll_bar,
         "LNX-04 concrete valuators have spin/slider/scrollbar roles");
  {
    DuringModal probe([&](Fl_Window &modal) {
      snapshot = tree.snapshot();
      auto *mode = control<editor::EditorChoice>(modal, "Color mode");
      auto *red = control<Fl_Value_Input>(modal, "Red");
      auto *green = control<Fl_Value_Input>(modal, "Green");
      auto *blue = control<Fl_Value_Input>(modal, "Blue");
      expect(mode && red && green && blue && control<Fl_Widget>(modal, "Hue and saturation") &&
                 control<Fl_Widget>(modal, "Value"), "LNX-04 native chooser controls have semantic names");
      if (mode && red && green && blue) {
        for (auto *channel : {red, green, blue}) {
          const auto *node = find(tree, snapshot, *channel);
          expect(node && node->role == a11y::Role::number_field,
                 "LNX-04 chooser channels are spin buttons");
        }
        mode->value(3); mode->do_callback(); snapshot = tree.snapshot();
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
  layerc::test::require_editor_window();
  scroll_focus_and_effective_extents();
  popup_semantics_and_recovery();
  text_dialog_layouts();
  named_color_chooser_and_roles();
}
