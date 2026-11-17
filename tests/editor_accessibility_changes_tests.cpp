#include "a11y/changes.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace layerc::editor::a11y;
using layerc::test::expect;

// Root window -> panel -> {profile name field, row}.
Snapshot fixture() {
  Snapshot tree;
  tree.root = 1;
  tree.generation = 1;
  Node root;
  root.id = 1;
  root.role = Role::window;
  root.name = "LayerE";
  root.bounds = {0.0, 0.0, 800.0, 600.0};
  root.actions = {"activate"};
  root.children = {2};
  root.states.active = true;
  root.states.showing = true;
  tree.nodes.emplace(1, root);
  Node panel;
  panel.id = 2;
  panel.parent = 1;
  panel.bounds = {0.0, 0.0, 400.0, 300.0};
  panel.children = {3, 4};
  tree.nodes.emplace(2, panel);
  Node field;
  field.id = 3;
  field.parent = 2;
  field.role = Role::text_field;
  field.name = "Profile name";
  field.bounds = {10.0, 20.0, 100.0, 24.0};
  field.text = Text{"Caf\xc3\xa9", 3, 3, {}, {}};
  field.actions = {"activate"};
  field.states.editable = true;
  field.states.focusable = true;
  tree.nodes.emplace(3, field);
  Node row;
  row.id = 4;
  row.parent = 2;
  row.role = Role::list_row;
  row.name = "Row";
  row.bounds = {10.0, 50.0, 100.0, 20.0};
  row.states.selectable = true;
  tree.nodes.emplace(4, row);
  return tree;
}

std::vector<ChangeKind> kinds(const std::vector<Change> &changes) {
  std::vector<ChangeKind> out;
  out.reserve(changes.size());
  for (const auto &change : changes) out.push_back(change.kind);
  return out;
}

const Change *first(const std::vector<Change> &changes, const ChangeKind kind) {
  const auto found = std::find_if(changes.begin(), changes.end(),
                                  [kind](const Change &change) {
                                    return change.kind == kind;
                                  });
  return found == changes.end() ? nullptr : &*found;
}

void empty_before_is_additions_only() {
  const Snapshot after = fixture();
  const auto out = changes(Snapshot{}, after);
  expect(out.size() == 4, "a first tree reports every node once");
  expect(kinds(out) == std::vector<ChangeKind>(4, ChangeKind::node_added),
         "a first tree reports additions only");
  expect(out[0].node == 1 && out[0].role == Role::window &&
             out[0].index == no_index,
         "root addition carries role and no index");
  expect(out[2].node == 3 && out[2].parent == 2 && out[2].index == 0,
         "field addition carries its parent and index");
  expect(changes(after, after).empty(), "identical snapshots report nothing");
}

void structure_changes_keep_order() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.erase(4);
  Node toggle;
  toggle.id = 5;
  toggle.parent = 2;
  toggle.role = Role::check_box;
  toggle.name = "Enabled";
  toggle.bounds = {10.0, 80.0, 100.0, 24.0};
  after.nodes.emplace(5, toggle);
  after.nodes.at(2).children = {5, 3}; // Toggle first, field reordered.
  after.nodes.at(1).children = {2};
  const auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::node_removed,
                                                ChangeKind::node_added,
                                                ChangeKind::child_removed,
                                                ChangeKind::child_removed,
                                                ChangeKind::child_added,
                                                ChangeKind::child_added}),
         "structure reports removals, additions, then parent-child changes");
  expect(out[0].node == 4 && out[0].parent == 2 && out[0].index == 1,
         "removed node keeps its old parent and index");
  expect(out[1].node == 5 && out[1].index == 0 && out[1].role == Role::check_box,
         "added node carries its new index and role");
  expect(out[2].node == 4 && out[2].parent == 2 && out[2].index == 1,
         "child removal reports the old index");
  expect(out[3].node == 3 && out[3].index == 0 && out[4].node == 5 &&
             out[4].index == 0 && out[5].node == 3 && out[5].index == 1,
         "a reordered child is removed at its old index and added at its new one");
}

void properties_and_states_in_order() {
  Snapshot before = fixture();
  Snapshot after = before;
  auto &field = after.nodes.at(3);
  field.name = "Name";
  field.description = "Profile name field";
  field.value = "value";
  field.states.focusable = false;
  field.states.checked = true;
  field.states.mixed = true;
  field.states.visible = false;
  const auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::name_changed,
                                                ChangeKind::description_changed,
                                                ChangeKind::value_changed,
                                                ChangeKind::state_changed,
                                                ChangeKind::state_changed,
                                                ChangeKind::state_changed,
                                                ChangeKind::state_changed}),
         "properties precede states in declared state order");
  expect(out[0].previous == "Profile name" && out[0].text == "Name",
         "name change carries both strings");
  expect(out[2].previous.empty() && out[2].text == "value" &&
             !out[2].from_text,
         "value change prefers Node::value");
  expect(out[3].state == StateKey::focusable && !out[3].state_value,
         "focusable flip reports the new flag");
  expect(out[4].state == StateKey::checked && out[4].state_value,
         "checked flip reports the new flag");
  expect(out[5].state == StateKey::mixed, "mixed flip is reported");
  expect(out[6].state == StateKey::visible && !out[6].state_value,
         "visible flip reports the new flag");
}

void text_value_and_spans() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.at(3).text->value = "Caf\xc3\xa9s";
  auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::value_changed,
                                                ChangeKind::text_inserted}),
         "text value change is followed by the inserted span");
  expect(out[0].from_text && out[0].previous == "Caf\xc3\xa9" &&
             out[0].text == "Caf\xc3\xa9s",
         "value change falls back to Text::value");
  expect(out[1].offset == 5 && out[1].text == "s",
         "insertion offset is a UTF-8 byte offset");

  after = before;
  after.nodes.at(3).text->value = "Caf\xc3\xa8"; // cafe, e-grave.
  out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::value_changed,
                                                ChangeKind::text_deleted,
                                                ChangeKind::text_inserted}),
         "replacements delete then insert at one offset");
  expect(out[1].offset == 3 && out[1].previous == "\xc3\xa9",
         "deletion snaps back to the code point boundary");
  expect(out[2].offset == 3 && out[2].text == "\xc3\xa8",
         "insertion shares the snapped offset");

  after = before;
  after.nodes.at(3).text->value = "Caf"; // Truncation.
  out = changes(before, after);
  expect(out.size() == 2 && out[1].kind == ChangeKind::text_deleted &&
             out[1].offset == 3 && out[1].previous == "\xc3\xa9",
         "truncation deletes the trailing code point");
}

void caret_and_selection() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.at(3).text->caret = 5;
  after.nodes.at(3).text->anchor = 5;
  auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::caret_moved,
                                                ChangeKind::selection_changed}),
         "a collapsed caret move reports both caret and selection");
  expect(out[0].offset == 5, "caret event carries the new byte offset");
  expect(out[1].anchor == 5 && out[1].caret == 5,
         "selection event carries anchor and caret");

  after = before;
  after.nodes.at(3).text->anchor = 1;
  out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::selection_changed}),
         "an anchor-only change reports the selection");
  expect(out[0].anchor == 1 && out[0].caret == 3,
         "selection event keeps the caret");
}

void focus_reports_one_node() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.at(3).states.focused = true;
  after.nodes.at(4).states.focused = true;
  auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::state_changed,
                                                ChangeKind::state_changed,
                                                ChangeKind::focus_moved}),
         "focus follows per-node state flips");
  expect(out.back().node == 4, "the last focused node in reading order wins");

  before.nodes.at(3).states.focused = true;
  after = before;
  after.nodes.at(4).states.focused = true;
  out = changes(before, after);
  expect(out.size() == 2 && out[0].kind == ChangeKind::state_changed &&
             out[1].kind == ChangeKind::focus_moved && out[1].node == 4,
         "an unchanged focus reports only the new one");

  after = before;
  out = changes(before, after);
  expect(out.empty(), "an unmoved focus reports nothing");
}

void windows_activate_and_deactivate() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.at(1).states.active = false;
  auto out = changes(before, after);
  expect(kinds(out) == std::vector<ChangeKind>({ChangeKind::state_changed,
                                                ChangeKind::window_deactivated}),
         "deactivating the window reports the window event");
  expect(out.back().node == 1, "window event names the window");

  Snapshot dialog_before = fixture();
  Node dialog;
  dialog.id = 5;
  dialog.parent = 1;
  dialog.role = Role::dialog;
  dialog.bounds = {100.0, 100.0, 200.0, 100.0};
  dialog_before.nodes.emplace(5, dialog);
  dialog_before.nodes.at(1).children = {2, 5};
  Snapshot dialog_after = dialog_before;
  dialog_after.nodes.at(1).states.active = false;
  dialog_after.nodes.at(5).states.active = true;
  out = changes(dialog_before, dialog_after);
  const auto activated = first(out, ChangeKind::window_activated);
  const auto deactivated = first(out, ChangeKind::window_deactivated);
  expect(activated != nullptr && activated->node == 5,
         "an active dialog activates");
  expect(deactivated != nullptr && deactivated->node == 1,
         "the window it replaces deactivates");

  after = before;
  after.nodes.erase(1);
  after.root = 0;
  out = changes(before, after);
  expect(first(out, ChangeKind::node_removed) != nullptr &&
             first(out, ChangeKind::window_deactivated) != nullptr,
         "a removed active window deactivates before it disappears");
}

void announcements_are_occurrences() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.announcement = "Profile saved";
  after.announcement_sequence = before.announcement_sequence + 1;
  after.announcement_source = 3;
  auto out = changes(before, after);
  expect(out.size() == 1 && out[0].kind == ChangeKind::announced &&
             out[0].node == 3 && out[0].text == "Profile saved",
         "a new announcement sequence reports the message and source");

  before.announcement_sequence = after.announcement_sequence;
  out = changes(before, after);
  expect(out.empty(), "the same sequence reports nothing again");

  before.announcement_sequence = after.announcement_sequence - 1;
  after.announcement.clear();
  out = changes(before, after);
  expect(out.empty(), "an empty announcement reports nothing");
}

void geometry_changes_are_silent() {
  Snapshot before = fixture();
  Snapshot after = before;
  after.nodes.at(3).bounds = {1.0, 2.0, 3.0, 4.0};
  after.nodes.at(3).text->runs.push_back(TextRun{0, 1, {1.0, 2.0, 3.0, 4.0}});
  after.nodes.at(3).text->caret_bounds = {5.0, 6.0, 7.0, 8.0};
  expect(changes(before, after).empty(),
         "bounds and caret geometry produce no events");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(empty_before_is_additions_only),
                 LAYERC_TEST_CASE(structure_changes_keep_order),
                 LAYERC_TEST_CASE(properties_and_states_in_order),
                 LAYERC_TEST_CASE(text_value_and_spans),
                 LAYERC_TEST_CASE(caret_and_selection),
                 LAYERC_TEST_CASE(focus_reports_one_node),
                 LAYERC_TEST_CASE(windows_activate_and_deactivate),
                 LAYERC_TEST_CASE(announcements_are_occurrences),
                 LAYERC_TEST_CASE(geometry_changes_are_silent))
