#pragma once

#include "model.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace layerc::editor::a11y {

// Every mutable States member. Flips are reported in this declaration order.
enum class StateKey {
  focused, focusable, selected, selectable, checked, mixed, expanded, collapsed,
  disabled, read_only, modal, showing, password, visible, active, resizable,
  editable, multi_line, checkable
};

enum class ChangeKind {
  // Structure: removals first, then additions, then parent-child changes, so
  // an adaptor has retracted and announced the nodes involved before it
  // reports where they moved.
  node_removed,
  node_added,
  child_removed,
  child_added,
  // Properties, states and text, in the after-snapshot reading order
  // (depth-first over children from the root, then any unreachable node by id).
  name_changed,
  description_changed,
  value_changed,
  range_value_changed,
  state_changed,
  text_deleted,
  text_inserted,
  caret_moved,
  text_selection_changed,
  container_selection_changed,
  // Focus and windows close the reading-order pass; the live announcement is
  // always last.
  focus_moved,
  window_activated,
  window_deactivated,
  announced
};

// Position inside a parent's child list is unavailable.
inline constexpr std::size_t no_index = static_cast<std::size_t>(-1);

// One protocol-neutral difference. Which fields apply depends on the kind:
//  node_removed, node_added      node, parent, index (old, new), role
//  child_removed, child_added    node (the child), parent, index (old, new)
//  name/description/value        node; previous, text; value also from_text
//  range_value_changed           node; number (new value)
//  state_changed                 node; state, state_value (the new flag)
//  text_deleted, text_inserted   node; offset (UTF-8 byte), previous or text
//  caret_moved                   node; offset (new UTF-8 byte caret)
//  text_selection_changed        node; anchor, caret (new UTF-8 bytes)
//  container_selection_changed   node (the container)
//  focus_moved                   node (the newly focused node)
//  window_activated/_deactivated node
//  announced                     node (the announcement source); text
struct Change {
  ChangeKind kind{ChangeKind::announced};
  NodeId node{no_node};
  NodeId parent{no_node};
  std::size_t index{no_index};
  Role role{Role::group};
  StateKey state{StateKey::focused};
  bool state_value{};
  bool from_text{};
  std::string previous;
  std::string text;
  std::size_t offset{};
  std::size_t anchor{};
  std::size_t caret{};
  double number{};
};

// Differences an adaptor turns into protocol events. Deterministic for equal
// snapshots; geometry changes produce nothing. Focus is reported for at most
// one node: when several nodes newly carry States::focused, the last one in
// the after-snapshot reading order wins.
//
// Emission order: node removals by id, node additions by id, parent-child
// changes per parent in reading order (removals before additions; a surviving
// child at a new index is removed at its old index and added at its new one),
// property/state/text changes per node in reading order, focus, windows, and
// finally the announcement.
[[nodiscard]] std::vector<Change> changes(const Snapshot &before,
                                          const Snapshot &after);

} // namespace layerc::editor::a11y
