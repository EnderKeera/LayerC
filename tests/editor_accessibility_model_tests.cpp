#include "a11y/model.hpp"
#include "test_support.hpp"

#include <limits>
#include <thread>

namespace {
using namespace layerc::editor::a11y;
using layerc::test::expect;

Snapshot fixture() {
  Snapshot tree;
  tree.root = 1;
  Node root;
  root.id = 1;
  root.role = Role::window;
  root.children = {2};
  tree.nodes.emplace(1, root);
  Node field;
  field.id = 2;
  field.parent = 1;
  field.role = Role::text_field;
  field.name = "Profile name";
  field.text = Text{"Caf\xc3\xa9", 5, 3};
  tree.nodes.emplace(2, field);
  return tree;
}

void validated_atomic_publication() {
  Model model;
  expect(!model.publish(fixture()), "valid tree publishes");
  auto bad = fixture();
  bad.nodes.at(2).text->caret = 4; // Inside é.
  expect(model.publish(bad).has_value(), "split UTF-8 offset rejected");
  expect(model.read().generation == 1, "invalid tree never commits");
  bad = fixture();
  bad.nodes.at(1).children.clear();
  expect(model.publish(bad).has_value(), "orphan rejected");
  bad = fixture();
  bad.nodes.at(2).parent = 2;
  bad.nodes.at(2).children = {2};
  expect(model.publish(bad).has_value(), "cycle rejected");
  bad = fixture();
  bad.nodes.at(2).labelled_by = {99};
  expect(model.publish(bad).has_value(), "dangling relation rejected");
  bad = fixture();
  bad.nodes.at(2).name = "\xff";
  expect(model.publish(bad).has_value(), "invalid UTF-8 rejected");
  bad = fixture();
  bad.nodes.at(2).range = Range{0, 10, 1, 11};
  expect(model.publish(bad).has_value(), "out-of-range number rejected");
}

void text_geometry_is_validated_atomically() {
  Model model;
  auto tree = fixture();
  tree.nodes.at(2).text->runs = {{0, 1, {0, 0, 10, 12}}, {3, 5, {10, 0, 10, 12}}};
  tree.nodes.at(2).text->caret_bounds = {20, 0, 1, 12};
  expect(!model.publish(tree), "ordered visible runs may omit offscreen text");
  for (int failure = 0; failure < 9; ++failure) {
    auto bad = tree;
    auto &text = *bad.nodes.at(2).text;
    switch (failure) {
    case 0: text.caret_bounds.left = std::numeric_limits<double>::quiet_NaN(); break;
    case 1: text.caret_bounds.height = -1; break;
    case 2: text.runs[1].bounds.width = -1; break;
    case 3: text.runs[1].bounds.top = std::numeric_limits<double>::infinity(); break;
    case 4: text.runs[1].end = 4; break; // Splits é.
    case 5: text.runs[1].begin = 4; break;
    case 6: text.runs[1].end = 6; break;
    case 7: text.runs[1].begin = 5; break;
    case 8: text.runs[1].begin = 0; break; // Overlaps the preceding run.
    }
    expect(model.publish(bad).has_value(), "invalid text geometry is rejected");
    const auto retained = model.read();
    expect(retained.generation == 1 && retained.nodes.at(2).text->runs[1].end == 5,
           "failed geometry publication leaves the complete prior tree intact");
  }
}

void protocol_neutral_semantics_are_validated_atomically() {
  Model model;
  auto tree = fixture();
  auto &window = tree.nodes.at(1);
  window.states.active = true;
  window.states.resizable = true;
  window.children.push_back(3);
  auto &field = tree.nodes.at(2);
  field.keybinding = "<Alt>p;;<Control>p";
  field.states.editable = true;
  field.states.multi_line = true;
  Node item;
  item.id = 3;
  item.parent = 1;
  item.role = Role::check_menu_item;
  item.actions = {"toggle"};
  item.states.checkable = true;
  item.states.selectable = true;
  item.states.checked = true;
  item.states.selected = true;
  tree.nodes.emplace(3, item);
  tree.announcement = "Saved";
  tree.announcement_sequence = 1;
  tree.announcement_source = 2;
  expect(!model.publish(tree), "portable roles, states, actions and announcement origin publish");
  for (int failure = 0; failure < 4; ++failure) {
    auto bad = tree;
    switch (failure) {
    case 0: bad.nodes.at(2).keybinding = "\xff"; break;
    case 1: bad.nodes.at(3).actions = {""}; break;
    case 2: bad.announcement_source = 99; break;
    case 3: bad.nodes.at(3).actions = {"\xff"}; break;
    }
    expect(model.publish(bad).has_value(), "invalid protocol metadata is rejected");
    const auto retained = model.read();
    expect(retained.generation == 1 && retained.announcement_source == 2 &&
               retained.nodes.at(2).keybinding == field.keybinding &&
               retained.nodes.at(3).actions == item.actions,
           "invalid metadata leaves the complete previously published snapshot intact");
  }
}

void wide_and_reordered_parent_links() {
  Model model;
  Snapshot wide;
  wide.root = 6000;
  Node root;
  root.id = wide.root;
  root.role = Role::application;
  for (NodeId id = 1; id <= 5843; ++id) {
    Node row;
    row.id = id;
    row.parent = root.id;
    row.role = Role::list_row;
    root.children.push_back(id);
    wide.nodes.emplace(id, row);
  }
  wide.nodes.emplace(root.id, root);
  expect(!model.publish(wide), "wide lists publish when parent IDs follow every child");
  expect(model.read().nodes.size() == 5844, "wide publication retains every row");
  auto bad = wide;
  bad.nodes.at(root.id).children.push_back(1);
  expect(model.publish(bad).has_value(), "duplicate children remain rejected");
  bad = wide;
  bad.nodes.at(root.id).children.pop_back();
  expect(model.publish(bad).has_value(), "wide-list orphan remains rejected");
  bad = wide;
  bad.nodes.at(3).children = {2};
  expect(model.publish(bad).has_value(), "a child cannot be listed by a second parent");
  expect(model.read().generation == 1, "invalid wide trees never replace the valid snapshot");

  Snapshot nested;
  nested.root = 99;
  Node top;
  top.id = 99;
  top.children = {20};
  Node middle;
  middle.id = 20;
  middle.parent = 99;
  middle.children = {2};
  Node leaf;
  leaf.id = 2;
  leaf.parent = 20;
  nested.nodes.emplace(2, leaf);
  nested.nodes.emplace(20, middle);
  nested.nodes.emplace(99, top);
  expect(!model.publish(nested), "reverse-ID ancestry validates after all child lists are visited");
}

void value_copy_boundaries() {
  Model model;
  expect(!model.publish(fixture()), "publish fixture");
  auto copy = model.read();
  copy.nodes.at(2).name = "Mutated";
  expect(model.read().nodes.at(2).name == "Profile name",
         "reader owns a copy, not live model state");
  std::thread writer([&] {
    for (int i = 0; i < 100; ++i) (void)model.publish(fixture());
  });
  bool consistent = true;
  for (int i = 0; i < 100; ++i) {
    const auto snapshot = model.read();
    consistent = consistent && snapshot.nodes.at(1).children.front() == 2 &&
                 snapshot.nodes.at(2).parent == 1;
  }
  writer.join();
  expect(consistent, "concurrent readers see complete snapshots");
  expect(model.read().generation == 101, "each commit advances generation");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(validated_atomic_publication),
                 LAYERC_TEST_CASE(text_geometry_is_validated_atomically),
                 LAYERC_TEST_CASE(protocol_neutral_semantics_are_validated_atomically),
                 LAYERC_TEST_CASE(wide_and_reordered_parent_links),
                 LAYERC_TEST_CASE(value_copy_boundaries))
