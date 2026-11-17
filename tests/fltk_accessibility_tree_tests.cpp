#include "a11y/fltk_actions.hpp"
#include "a11y/fltk_tree.hpp"
#include "a11y/model.hpp"
#include "a11y/input.hpp"
#include "a11y/window_geometry.hpp"
#include "accessible_widgets.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Radio_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace {
namespace a11y = layerc::editor::a11y;
namespace accessibility = layerc::editor::accessibility;
using layerc::test::expect;

// Large enough that most rows never fit the viewport.
constexpr int browser_rows = 5843;

bool finite(const a11y::Bounds &bounds) {
  return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
         std::isfinite(bounds.width) && std::isfinite(bounds.height);
}

struct Fixture {
  Fl_Window window{0, 0, 640, 400, "Tree probe"};
  Fl_Input field{10, 10, 200, 24, "&Field"};
  Fl_Slider slider{10, 40, 200, 24, "Level"};
  Fl_Button button{10, 70, 200, 24, "&Apply"};
  Fl_Input secret{220, 10, 120, 24, "Secret"};
  Fl_Input raw{350, 10, 120, 24, "Raw"};
  Fl_Tabs tabs{10, 100, 300, 220};
  Fl_Group rules{10, 125, 300, 195, "Rules"};
  Fl_Browser list{20, 150, 280, 160};
  Fl_Group other{10, 125, 300, 195, "Other"};
  Fl_Box other_label{20, 150, 280, 24, "Nothing here"};

  Fixture() {
    window.add(field);
    window.add(slider);
    window.add(button);
    window.add(secret);
    window.add(raw);
    secret.type(FL_SECRET_INPUT);
    secret.value("hunter2");
    raw.value("bad \xff byte");
    window.add(tabs);
    tabs.begin();
    tabs.add(rules);
    tabs.add(other);
    tabs.end();
    rules.begin();
    rules.add(list);
    rules.end();
    other.begin();
    other.add(other_label);
    other.end();
    field.value("alpha beta");
    field.insert_position(5, 0);
    slider.bounds(0, 100);
    slider.step(1);
    slider.value(40);
    button.shortcut(FL_ALT | 'a');
    for (int row = 1; row <= browser_rows; ++row) list.add(("row " + std::to_string(row)).c_str());
    tabs.value(&rules);
    window.end();
  }

  void show() {
    window.show();
    Fl::check();
  }
};

// The node a widget's own identity exported, or nullptr when it has none.
const a11y::Node *node_of(const a11y::Snapshot &snapshot, a11y::NodeId id) {
  const auto found = snapshot.nodes.find(id);
  return found == snapshot.nodes.end() ? nullptr : &found->second;
}

const a11y::Node *widget_node(a11y::FltkTree &tree, const a11y::Snapshot &snapshot,
                              const Fl_Widget &widget) {
  return node_of(snapshot, tree.id_for(widget));
}

bool children_are_consistent(const a11y::Snapshot &snapshot) {
  std::size_t roots{};
  for (const auto &[id, node] : snapshot.nodes) {
    if (node.id != id) return false;
    if (id == snapshot.root) {
      if (node.parent != a11y::no_node) return false;
      ++roots;
      continue;
    }
    const auto *parent = node_of(snapshot, node.parent);
    if (parent == nullptr) return false;
    bool listed{};
    for (const auto child : parent->children) listed = listed || child == id;
    if (!listed) return false;
    for (const auto child : node.children) {
      const auto *found = node_of(snapshot, child);
      if (found == nullptr || found->parent != id) return false;
    }
  }
  return roots == 1;
}
} // namespace

void fltk_accessibility_tree_snapshots() {
#ifndef _WIN32
  layerc::test::require_x11();
#endif
  Fixture fixture;
  accessibility::set_accessible_name(fixture.field, "Source filter");
  accessibility::set_accessible_description(fixture.field, "Filters the rules by source");
  fixture.show();
  if (!expect(fixture.window.shown(), "the probe window is on screen")) return;

  a11y::FltkTree tree;
  a11y::Model model;

  // The snapshot is a tree the model accepts, and it is not published by the
  // tree itself.
  auto first = tree.snapshot();
  if (!expect(model.publish(first) == std::nullopt, "the tree publishes to the model")) return;
  if (!expect(children_are_consistent(first), "parent and child relations are consistent")) return;
  if (!expect(!model.read().nodes.empty(), "the model holds the published nodes")) return;

  const auto *root = node_of(first, first.root);
  if (!expect(root != nullptr, "the snapshot is rooted")) return;
  expect(root->role == a11y::Role::application, "the root is the application");
  expect(root->parent == a11y::no_node, "the root has no parent");
  expect(root->name == "LayerE", "the application name is unchanged");

  // Widgets keep their identity, role and order.
  const auto window_id = tree.id_for(fixture.window);
  const auto field_id = tree.id_for(fixture.field);
  const auto button_id = tree.id_for(fixture.button);
  const auto slider_id = tree.id_for(fixture.slider);
  const auto list_id = tree.id_for(fixture.list);
  if (!expect(window_id != a11y::no_node && field_id != a11y::no_node &&
                   button_id != a11y::no_node && slider_id != a11y::no_node &&
                   list_id != a11y::no_node,
               "every probe widget exported")) return;
  const auto *window_node = node_of(first, window_id);
  const auto *field_node = node_of(first, field_id);
  const auto *button_node = node_of(first, button_id);
  const auto *slider_node = node_of(first, slider_id);
  const auto *list_node = node_of(first, list_id);
  if (!expect(window_node && field_node && button_node && slider_node && list_node,
               "every probe node is present")) return;
  expect(window_node->parent == first.root, "a top-level window hangs off the application");
  expect(window_node->role == a11y::Role::window, "a window keeps its role");
  expect(field_node->role == a11y::Role::text_field, "an input keeps its role");
  expect(button_node->role == a11y::Role::button, "a button keeps its role");
  expect(slider_node->role == a11y::Role::slider, "a slider keeps its role");
  expect(list_node->role == a11y::Role::list, "a browser keeps its role");

  // Registered names and descriptions win, actions and keybindings export.
  expect(field_node->name == "Source filter", "a registered name exports");
  expect(field_node->description == "Filters the rules by source", "a registered description exports");
  expect(field_node->actions.empty(), "a plain input has no action interface");
  expect(button_node->actions.size() == 1 && button_node->actions.front() == "activate",
         "a button exports its activation");
  expect(!button_node->keybinding.empty(), "a shortcut exports its keybinding");
  for (const auto &[id, node] : first.nodes) {
    (void)id;
    expect(finite(node.bounds), "every node has finite geometry");
  }

  // Text geometry and the one selection the field reports.
  if (!expect(field_node->text.has_value(), "an input exports its text")) return;
  expect(field_node->text->value == "alpha beta", "the input text is the field's value");
  expect(field_node->text->caret == 5 && field_node->text->anchor == 0,
         "caret and anchor are byte offsets");
  expect(!field_node->text->runs.empty(), "the input exports text runs");
  expect(finite(field_node->text->caret_bounds), "the caret geometry is finite");
  const auto origin = a11y::window_screen_origin(fixture.window);
  const auto local_text = a11y::field_text(fixture.field);
  if (!expect(origin.has_value() && local_text.has_value(), "screen text geometry is available"))
    return;
  const auto scale = static_cast<double>(Fl::screen_scale(fixture.window.screen_num()));
  expect(field_node->text->caret_bounds.left ==
             origin->first + std::round(local_text->caret_bounds.left * scale) &&
             field_node->text->caret_bounds.top ==
             origin->second + std::round(local_text->caret_bounds.top * scale),
         "the caret is in physical screen coordinates");
  expect(field_node->value == "alpha beta", "the field exports its string value");
  bool runs_inside = true;
  std::size_t previous_end{};
  for (const auto &run : field_node->text->runs) {
    runs_inside = runs_inside && run.begin >= previous_end && run.begin < run.end &&
                  run.end <= field_node->text->value.size() && finite(run.bounds);
    previous_end = run.end;
  }
  expect(runs_inside, "text runs are ordered and inside the value");

  // Secret and undecodable fields keep their node but export no text, and
  // neither stops the rest of the snapshot from publishing.
  const auto *secret_node = widget_node(tree, first, fixture.secret);
  const auto *raw_node = widget_node(tree, first, fixture.raw);
  if (!expect(secret_node && raw_node, "secret and raw fields are exported")) return;
  expect(secret_node->states.password, "a secret field is a password");
  expect(!secret_node->text && secret_node->value.empty(), "a secret field exports no text");
  expect(!raw_node->text && raw_node->value.empty(), "undecodable text exports no text interface");

  // A valuator exports its range, which the model keeps in bounds.
  if (!expect(slider_node->range.has_value(), "a slider exports its range")) return;
  expect(slider_node->range->minimum == 0 && slider_node->range->maximum == 100 &&
             slider_node->range->value == 40,
         "the slider range matches the widget");

  // A visible list exports every row, including the ones scrolled out of view.
  if (!expect(list_node->states.showing, "the visible page's browser is showing")) return;
  if (!expect(list_node->children.size() == static_cast<std::size_t>(browser_rows),
               "a visible browser exports every row")) return;
  const auto first_row = list_node->children.front();
  const auto last_row = list_node->children.back();
  const auto *last_node = node_of(first, last_row);
  if (!expect(last_node != nullptr, "the last row is exported")) return;
  expect(last_node->role == a11y::Role::list_row, "rows keep their role");
  expect(last_node->name == "row 5843", "row text is the row name");

  // An unchanged pass keeps every identity, including the rows.
  auto second = tree.snapshot();
  expect(tree.id_for(fixture.window) == window_id, "a window keeps its id");
  expect(tree.id_for(fixture.list) == list_id, "a list keeps its id");
  expect(tree.id_for(fixture.field) == field_id, "an input keeps its id");
  const auto *second_list = node_of(second, list_id);
  if (!expect(second_list != nullptr, "the list survives the second pass")) return;
  expect(second_list->children.size() == list_node->children.size(), "the row count is stable");
  expect(second_list->children.front() == first_row && second_list->children.back() == last_row,
         "row ids are stable");

  // A hidden page keeps its widgets, drops its rows and zeroes their geometry.
  fixture.tabs.value(&fixture.other);
  auto hidden = tree.snapshot();
  const auto *hidden_list = node_of(hidden, list_id);
  const auto *shown_label = widget_node(tree, hidden, fixture.other_label);
  if (!expect(hidden_list != nullptr && shown_label != nullptr, "page widgets are exported")) return;
  expect(hidden_list->children.empty(), "a hidden page's browser exports no rows");
  expect(tree.id_for(fixture.list) == list_id,
         "retiring rows does not retire their browser's widget identity");
  expect(!hidden_list->states.showing, "a hidden page's browser is not showing");
  expect(finite(hidden_list->bounds), "a hidden widget keeps finite geometry");
  expect(shown_label->states.showing, "the shown page's label is showing");

  // Rows that return are new targets: retired ids never revive.
  fixture.tabs.value(&fixture.rules);
  auto shown_again = tree.snapshot();
  const auto *again_list = node_of(shown_again, list_id);
  if (!expect(again_list != nullptr, "the browser returns")) return;
  if (!expect(again_list->children.size() == static_cast<std::size_t>(browser_rows),
               "the returning list exports every row again")) return;
  expect(again_list->children.front() != first_row, "a returning row gets a new id");
  expect(tree.id_for(fixture.list) == list_id, "the returning browser keeps its widget identity");
  expect(shown_again.nodes.find(first_row) == shown_again.nodes.end(),
         "a retired row id stays retired");
  expect(tree.target(first_row) == std::nullopt, "a retired target is rejected");
  const auto *returned_label = widget_node(tree, shown_again, fixture.other_label);
  if (!expect(returned_label != nullptr, "the label survives the page switch")) return;
  expect(!returned_label->states.showing, "a hidden page's label is not showing");

  // A deleted widget's id names nothing, even before the next pass.
  auto *scratch = new Fl_Window(700, 0, 240, 140, "Scratch");
  scratch->begin();
  auto *label = new Fl_Box(10, 10, 120, 24, "Scratch label");
  (void)label;
  scratch->end();
  scratch->show();
  Fl::check();
  auto with_scratch = tree.snapshot();
  const auto scratch_id = tree.id_for(*scratch);
  expect(scratch_id != a11y::no_node, "a second window exports");
  expect(with_scratch.nodes.find(scratch_id) != with_scratch.nodes.end(),
         "the second window is in the snapshot");
  delete scratch;
  Fl::check();
  expect(tree.target(scratch_id) == std::nullopt, "a deleted widget's target is rejected");
  auto after_delete = tree.snapshot();
  expect(after_delete.nodes.find(scratch_id) == after_delete.nodes.end(),
         "a deleted window leaves the snapshot");
  expect(after_delete.root == shown_again.root, "the application keeps its id");

  // clear() forgets every identity and continues the sequence.
  tree.clear();
  expect(tree.target(window_id) == std::nullopt, "cleared ids name nothing");
  auto cleared = tree.snapshot();
  expect(cleared.root != first.root, "a cleared tree allocates a new root");
  expect(tree.id_for(fixture.window) != window_id, "a cleared tree allocates new widget ids");
  expect(cleared.nodes.find(window_id) == cleared.nodes.end(), "an old window id is gone");
  expect(children_are_consistent(cleared), "the rebuilt tree is consistent");

  model.clear();
  expect(model.read().nodes.empty(), "the model clears");
  fixture.window.hide();
}

// Actions validate on the calling thread and run their application callback from
// FLTK's loop, so the protocol call returns before a dialog can open.
void fltk_accessibility_actions() {
#ifndef _WIN32
  layerc::test::require_x11();
#endif
  const auto count = [](Fl_Widget *, void *data) { ++*static_cast<int *>(data); };
  const auto turn = [] { for (int round = 0; round < 3; ++round) Fl::check(); };

  Fl_Window window(0, 0, 560, 420, "Action probe");
  Fl_Input field(20, 20, 220, 24, "Text");
  Fl_Check_Button toggle(20, 60, 220, 24, "Toggle");
  Fl_Group radios(20, 95, 220, 65);
  Fl_Radio_Button first_radio(30, 100, 200, 24, "First");
  Fl_Radio_Button second_radio(30, 130, 200, 24, "Second");
  radios.end();
  Fl_Tabs tabs(20, 180, 220, 130);
  Fl_Group left_page(20, 210, 220, 100, "Left");
  left_page.end();
  Fl_Group right_page(20, 210, 220, 100, "Right");
  right_page.end();
  tabs.end();
  Fl_Hold_Browser list(260, 20, 280, 120);
  Fl_Button apply(260, 160, 280, 24, "Apply");
  Fl_Slider slider(260, 200, 280, 24, "Level");
  window.end();
  slider.bounds(0, 100);
  slider.step(1);
  slider.value(40);
  for (int row = 1; row <= 3; ++row) list.add(("row " + std::to_string(row)).c_str());
  tabs.value(&left_page);
  window.show();
  Fl::check();
  if (!expect(window.shown(), "the action window is on screen")) return;

  const a11y::Target apply_target{&apply, a11y::TargetKind::widget, 0};
  int apply_calls{};
  apply.callback(count, &apply_calls);
  expect(a11y::activate(apply_target), "an active button accepts activation");
  expect(apply_calls == 0, "activation runs no callback inside the call");
  turn();
  expect(apply_calls == 1, "activation runs the callback from the loop");
  apply.deactivate();
  expect(!a11y::activate(apply_target), "an inactive button rejects activation");
  turn();
  expect(apply_calls == 1, "a rejected activation queues nothing");
  apply.activate();

  // A deleted widget and a cancelled queue drop their steps.
  auto *scratch = new Fl_Button(20, 320, 220, 24, "Scratch");
  int scratch_calls{};
  scratch->callback(count, &scratch_calls);
  expect(a11y::activate(a11y::Target{scratch, a11y::TargetKind::widget, 0}),
         "a queued activation is accepted");
  delete scratch;
  turn();
  expect(scratch_calls == 0, "a deleted widget's queued step is dropped");
  auto *cancelled = new Fl_Button(20, 350, 220, 24, "Cancelled");
  int cancelled_calls{};
  cancelled->callback(count, &cancelled_calls);
  expect(a11y::activate(a11y::Target{cancelled, a11y::TargetKind::widget, 0}),
         "a second queued activation is accepted");
  a11y::cancel_deferred();
  turn();
  expect(cancelled_calls == 0, "cancel_deferred drops the queued step");
  delete cancelled;
  expect(a11y::activate(apply_target) && apply_calls == 1, "the queue resumes after a cancel");
  turn();
  expect(apply_calls == 2, "the resumed step runs from the loop");

  // Buttons act like their own click, tabs like a page pick, rows like a select.
  int toggle_calls{};
  toggle.callback(count, &toggle_calls);
  expect(a11y::activate(a11y::Target{&toggle, a11y::TargetKind::widget, 0}),
         "a check box accepts activation");
  expect(toggle.value() == 0, "a check box flips from the loop");
  turn();
  expect(toggle.value() == 1 && toggle_calls == 1, "a check box toggles and calls back");

  int radio_calls{};
  second_radio.callback(count, &radio_calls);
  first_radio.setonly();
  expect(a11y::activate(a11y::Target{&second_radio, a11y::TargetKind::widget, 0}),
         "a radio button accepts activation");
  turn();
  expect(second_radio.value() == 1 && first_radio.value() == 0, "a radio button takes the group");
  expect(radio_calls == 1, "the radio button's callback runs once");

  int tab_calls{};
  tabs.callback(count, &tab_calls);
  expect(a11y::activate(a11y::Target{&right_page, a11y::TargetKind::widget, 0}),
         "a page tab accepts activation");
  turn();
  expect(tabs.value() == &right_page, "the tab strip shows the chosen page");
  expect(tab_calls == 1, "the tab strip's callback runs once");

  int list_calls{};
  list.callback(count, &list_calls);
  expect(a11y::activate(a11y::Target{&list, a11y::TargetKind::row, 2}),
         "a browser row accepts activation");
  expect(list.selected(2) == 0, "a row selects from the loop");
  turn();
  expect(list.selected(2) != 0 && list.value() == 2, "the browser selects the row");
  expect(list_calls == 1, "the browser's callback runs once");
  expect(a11y::activate(a11y::Target{&list, a11y::TargetKind::row, 9}),
         "a row past the list still queues, then drops");
  turn();
  expect(list_calls == 1, "a row past the list calls nothing back");

  // A modal window stops steps aimed at another window.
  Fl_Window modal_window(600, 0, 200, 120, "Modal probe");
  modal_window.set_modal();
  modal_window.show();
  Fl::check();
  if (!expect(Fl::modal() == &modal_window, "the modal window owns the modal state")) return;
  const int before_modal = apply_calls;
  expect(!a11y::activate(apply_target), "a widget behind a modal window rejects activation");
  turn();
  expect(apply_calls == before_modal, "a modal window stops the queued step");
  modal_window.hide();
  Fl::check();
  expect(Fl::modal() == nullptr, "hiding the modal window releases it");

  // Text offsets are UTF-8 byte offsets: "a\xe2\x82\xac" "b" is 61 e2 82 ac 62.
  const a11y::Target text_target{&field, a11y::TargetKind::widget, 0};
  field.value("a\xe2\x82\xac" "b");
  expect(!a11y::set_caret(text_target, 2), "a caret inside a code point is rejected");
  expect(!a11y::set_caret(text_target, 6), "a caret past the end is rejected");
  expect(a11y::set_caret(text_target, 4) && field.insert_position() == 4,
         "a caret on a boundary moves");
  expect(a11y::select_text(text_target, 1, 4), "a selection on boundaries is accepted");
  expect(field.insert_position() == 4 && field.mark() == 1,
         "the selection reports its anchor and caret");
  expect(!a11y::select_text(text_target, 1, 2), "a selection ending inside a code point is rejected");

  field.value("a\xe2\x82\xac" "b");
  int field_calls{};
  field.callback(count, &field_calls);
  expect(a11y::insert_text(text_target, 1, "X"), "an insertion on a boundary is accepted");
  expect(std::string(field.value()) == "aX\xe2\x82\xac" "b", "an insertion lands immediately");
  expect(field.insert_position() == 2, "an insertion moves the caret past the text");
  expect(field_calls == 0, "an edit runs no callback inside the call");
  turn();
  expect(field_calls == 1, "an edit runs the field's callback from the loop");
  expect(a11y::delete_text(text_target, 1, 2), "a deletion on boundaries is accepted");
  expect(std::string(field.value()) == "a\xe2\x82\xac" "b", "a deletion lands immediately");
  expect(a11y::set_text(text_target, "plain"), "a whole value is accepted");
  expect(std::string(field.value()) == "plain", "a whole value lands immediately");
  turn();
  expect(field_calls == 3, "each edit calls back once");
  field.value("a\xe2\x82\xac" "b");
  expect(!a11y::insert_text(text_target, 2, "X"), "an insertion inside a code point is rejected");
  expect(!a11y::delete_text(text_target, 0, 9), "a deletion past the end is rejected");
  turn();
  expect(field_calls == 3, "a rejected edit calls nothing back");

  field.readonly(1);
  expect(!a11y::set_text(text_target, "other") && !a11y::insert_text(text_target, 0, "x") &&
             !a11y::delete_text(text_target, 0, 1),
         "a read-only field rejects every edit");
  turn();
  expect(field_calls == 3 && std::string(field.value()) == "a\xe2\x82\xac" "b",
         "a read-only field keeps its value");
  field.readonly(0);
  field.value("bad \xff byte");
  expect(!a11y::set_caret(text_target, 0) && !a11y::set_text(text_target, "ok"),
         "a field whose value is not UTF-8 is rejected");

  // Valuators take a bounded number, and only a change calls back.
  const a11y::Target slider_target{&slider, a11y::TargetKind::widget, 0};
  int slider_calls{};
  slider.callback(count, &slider_calls);
  expect(!a11y::set_number(slider_target, 120.0), "a value past the range is rejected");
  expect(!a11y::set_number(slider_target, -1.0), "a value below the range is rejected");
  expect(!a11y::set_number(slider_target, std::numeric_limits<double>::quiet_NaN()),
         "a NaN value is rejected");
  expect(a11y::set_number(slider_target, 55.5), "an in-range value is accepted");
  expect(slider.value() == 55.5, "the valuator takes the value immediately");
  expect(slider_calls == 0, "a valuator change runs no callback inside the call");
  turn();
  expect(slider_calls == 1, "a valuator change calls back from the loop");
  expect(a11y::set_number(slider_target, 55.5), "an unchanged value is still accepted");
  turn();
  expect(slider_calls == 1, "an unchanged value queues no callback");

  window.hide();
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(fltk_accessibility_tree_snapshots),
                 LAYERC_TEST_CASE(fltk_accessibility_actions))
