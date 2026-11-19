#include "model.hpp"

#include "layerc/core/utf8.hpp"

#include <cmath>
#include <cstddef>
#include <set>
#include <string_view>
#include <utility>

namespace layerc::editor::a11y {
namespace {
bool valid_bounds(const Bounds &bounds) noexcept {
  return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
         std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
         bounds.width >= 0 && bounds.height >= 0;
}

std::optional<ModelError> validate(const Snapshot &snapshot) {
  const auto error = [](NodeId id, const char *message) {
    return std::optional<ModelError>{{id, message}};
  };
  const auto root = snapshot.nodes.find(snapshot.root);
  if (root == snapshot.nodes.end() || snapshot.root == no_node ||
      root->second.parent != no_node)
    return error(snapshot.root, "accessibility tree has no valid root");
  if (!core::is_valid_utf8(snapshot.announcement))
    return error(snapshot.root, "invalid announcement UTF-8");
  if (snapshot.announcement_source != no_node &&
      !snapshot.nodes.contains(snapshot.announcement_source))
    return error(snapshot.root, "dangling announcement source");
  std::set<NodeId> attached;
  for (const auto &[id, node] : snapshot.nodes) {
    if (id == no_node || id != node.id)
      return error(id, "invalid accessibility node identity");
    if (!core::is_valid_utf8(node.name) ||
        !core::is_valid_utf8(node.description) ||
        !core::is_valid_utf8(node.value) ||
        !core::is_valid_utf8(node.keybinding))
      return error(id, "invalid accessibility text UTF-8");
    for (const auto &action : node.actions)
      if (action.empty() || !core::is_valid_utf8(action))
        return error(id, "invalid accessibility action name");
    if (!valid_bounds(node.bounds))
      return error(id, "invalid accessibility bounds");
    if (node.states.expanded && node.states.collapsed)
      return error(id, "contradictory accessibility expansion state");
    if (node.text) {
      const auto &text = *node.text;
      if (!core::is_valid_utf8(text.value) || text.caret > text.value.size() ||
          text.anchor > text.value.size() ||
          !core::is_valid_utf8(std::string_view(text.value).substr(0, text.caret)) ||
          !core::is_valid_utf8(std::string_view(text.value).substr(0, text.anchor)))
        return error(id, "invalid accessibility text selection");
      if (!valid_bounds(text.caret_bounds))
        return error(id, "invalid accessibility caret bounds");
      std::size_t previous_end{};
      for (const auto &run : text.runs) {
        if (run.begin < previous_end || run.begin >= run.end ||
            run.end > text.value.size() || !valid_bounds(run.bounds) ||
            !core::is_valid_utf8(std::string_view(text.value).substr(
                run.begin, run.end - run.begin)))
          return error(id, "invalid accessibility text run");
        previous_end = run.end;
      }
    }
    if (node.range) {
      const auto &r = *node.range;
      if (!std::isfinite(r.minimum) || !std::isfinite(r.maximum) ||
          !std::isfinite(r.step) || !std::isfinite(r.value) ||
          r.minimum > r.maximum || r.step < 0 ||
          r.value < r.minimum || r.value > r.maximum)
        return error(id, "invalid accessibility numeric range");
    }
    if (node.scroll) {
      const auto &s = *node.scroll;
      // -1 marks an axis that cannot scroll; every other percentage is a
      // position inside the content, and view shares are always fractions.
      const auto percentage = [](double value) {
        return std::isfinite(value) && (value == -1 || (value >= 0 && value <= 100));
      };
      if (!percentage(s.horizontal) || !percentage(s.vertical) ||
          !percentage(s.horizontal_view) || !percentage(s.vertical_view))
        return error(id, "invalid accessibility scroll state");
    }
    for (const auto child : node.children) {
      const auto found = snapshot.nodes.find(child);
      if (!attached.insert(child).second || found == snapshot.nodes.end() ||
          found->second.parent != id)
        return error(id, "invalid accessibility child relation");
    }
    for (const auto &relations : {node.labelled_by, node.described_by})
      for (const auto relation : relations)
        if (!snapshot.nodes.contains(relation))
          return error(id, "dangling accessibility relation");
    if (id != snapshot.root) {
      const auto parent = snapshot.nodes.find(node.parent);
      if (parent == snapshot.nodes.end())
        return error(id, "missing accessibility parent relation");
      auto ancestor = node.parent;
      std::size_t depth{};
      while (ancestor != no_node) {
        const auto found = snapshot.nodes.find(ancestor);
        if (found == snapshot.nodes.end() || ++depth > snapshot.nodes.size())
          return error(id, "cyclic accessibility parent relation");
        ancestor = found->second.parent;
      }
    }
  }
  // Check reverse links only after every parent's list has been visited. IDs
  // need not be ordered by ancestry, and a wide list must not rebuild its
  // complete child set once for every row.
  for (const auto &[id, node] : snapshot.nodes) {
    (void)node;
    if (id != snapshot.root && !attached.contains(id))
      return error(id, "missing accessibility parent relation");
  }
  return std::nullopt;
}
} // namespace

std::optional<ModelError> Model::publish(Snapshot snapshot) {
  if (auto error = validate(snapshot)) return error;
  const std::lock_guard lock(mutex_);
  snapshot.generation = snapshot_.generation + 1;
  snapshot_ = std::move(snapshot);
  return std::nullopt;
}

Snapshot Model::read() const {
  const std::lock_guard lock(mutex_);
  return snapshot_;
}

void Model::clear() {
  const std::lock_guard lock(mutex_);
  snapshot_ = {};
}

std::optional<Node> Model::read(const NodeId id) const {
  const std::lock_guard lock(mutex_);
  const auto found = snapshot_.nodes.find(id);
  if (found == snapshot_.nodes.end()) return std::nullopt;
  return found->second;
}

} // namespace layerc::editor::a11y
