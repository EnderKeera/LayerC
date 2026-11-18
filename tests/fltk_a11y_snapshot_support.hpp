#pragma once

// Reading the portable accessibility snapshot the way the old ATK tests read
// their objects: find a node by the widget (and row or item) that owns it, then
// ask about its states, bounds, and actions. The snapshot is the same model the
// AT-SPI and UIA bridges publish, so a regression here is a regression for every
// screen reader.

#include "a11y/fltk_actions.hpp"
#include "a11y/fltk_tree.hpp"
#include "a11y/model.hpp"

#include <FL/Fl_Widget.H>

#include <array>
#include <cstddef>

namespace layerc::test::a11y_snapshot {

namespace a11y = layerc::editor::a11y;

using Bounds = std::array<double, 4>;

inline const a11y::Node *lookup(const a11y::Snapshot &snapshot, const a11y::NodeId id) {
  const auto found = snapshot.nodes.find(id);
  return found == snapshot.nodes.end() ? nullptr : &found->second;
}

// The node `widget` owns, or the row or menu item it publishes at `index`.
inline const a11y::Node *find(a11y::FltkTree &tree, const a11y::Snapshot &snapshot,
                              const Fl_Widget &widget,
                              const a11y::TargetKind kind = a11y::TargetKind::widget,
                              const int index = 0) {
  for (const auto &[id, node] : snapshot.nodes) {
    (void)node;
    const auto target = tree.target(id);
    if (!target || target->widget != &widget || target->kind != kind ||
        target->index != index)
      continue;
    if (const auto *found = lookup(snapshot, id)) return found;
  }
  return nullptr;
}

inline const a11y::Node *child(const a11y::Snapshot &snapshot,
                               const a11y::Node &parent, const std::size_t index) {
  return index < parent.children.size() ? lookup(snapshot, parent.children[index])
                                        : nullptr;
}

inline Bounds bounds(const a11y::Node *node) {
  return node ? Bounds{node->bounds.left, node->bounds.top, node->bounds.width,
                       node->bounds.height}
              : Bounds{};
}

inline bool showing(const a11y::Node *node) { return node && node->states.showing; }
inline bool focused(const a11y::Node *node) { return node && node->states.focused; }
inline bool selected(const a11y::Node *node) { return node && node->states.selected; }
inline bool enabled(const a11y::Node *node) {
  return node && !node->states.disabled;
}

// Runs the node's own action the way a screen reader would.
inline bool activate(a11y::FltkTree &tree, const a11y::Node *node) {
  if (!node) return false;
  const auto target = tree.target(node->id);
  return target && a11y::activate(*target);
}

} // namespace layerc::test::a11y_snapshot
