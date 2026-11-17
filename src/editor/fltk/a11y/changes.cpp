#include "a11y/changes.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string_view>

namespace layerc::editor::a11y {
namespace {

constexpr StateKey all_states[] = {
    StateKey::focused,    StateKey::focusable, StateKey::selected,
    StateKey::selectable, StateKey::checked,   StateKey::mixed,
    StateKey::expanded,   StateKey::collapsed, StateKey::disabled,
    StateKey::read_only,  StateKey::modal,     StateKey::showing,
    StateKey::password,   StateKey::visible,   StateKey::active,
    StateKey::resizable,  StateKey::editable,  StateKey::multi_line,
    StateKey::checkable};

bool flag(const States &states, StateKey key) {
  switch (key) {
  case StateKey::focused: return states.focused;
  case StateKey::focusable: return states.focusable;
  case StateKey::selected: return states.selected;
  case StateKey::selectable: return states.selectable;
  case StateKey::checked: return states.checked;
  case StateKey::mixed: return states.mixed;
  case StateKey::expanded: return states.expanded;
  case StateKey::collapsed: return states.collapsed;
  case StateKey::disabled: return states.disabled;
  case StateKey::read_only: return states.read_only;
  case StateKey::modal: return states.modal;
  case StateKey::showing: return states.showing;
  case StateKey::password: return states.password;
  case StateKey::visible: return states.visible;
  case StateKey::active: return states.active;
  case StateKey::resizable: return states.resizable;
  case StateKey::editable: return states.editable;
  case StateKey::multi_line: return states.multi_line;
  case StateKey::checkable: return states.checkable;
  }
  return false;
}

// Depth-first over children from the root, then any unreachable node by id.
std::vector<NodeId> reading_order(const Snapshot &snapshot) {
  std::vector<NodeId> order;
  std::set<NodeId> seen;
  std::vector<NodeId> pending;
  if (snapshot.nodes.contains(snapshot.root)) pending.push_back(snapshot.root);
  while (!pending.empty()) {
    const NodeId id = pending.back();
    pending.pop_back();
    if (!seen.insert(id).second) continue;
    const auto node = snapshot.nodes.find(id);
    if (node == snapshot.nodes.end()) continue;
    order.push_back(id);
    for (auto child = node->second.children.rbegin();
         child != node->second.children.rend(); ++child)
      pending.push_back(*child);
  }
  for (const auto &[id, node] : snapshot.nodes) {
    (void)node;
    if (!seen.contains(id)) order.push_back(id);
  }
  return order;
}

std::optional<std::size_t> child_index(const Snapshot &snapshot, NodeId id) {
  const auto node = snapshot.nodes.find(id);
  if (node == snapshot.nodes.end() || node->second.parent == no_node)
    return std::nullopt;
  const auto parent = snapshot.nodes.find(node->second.parent);
  if (parent == snapshot.nodes.end()) return std::nullopt;
  const auto &children = parent->second.children;
  const auto position = std::find(children.begin(), children.end(), id);
  if (position == children.end()) return std::nullopt;
  return static_cast<std::size_t>(position - children.begin());
}

Change entry(ChangeKind kind, NodeId node) {
  Change change;
  change.kind = kind;
  change.node = node;
  return change;
}

void structure(const Snapshot &before, const Snapshot &after,
               std::vector<Change> &out) {
  for (const auto &[id, node] : before.nodes) {
    if (after.nodes.contains(id)) continue;
    Change change = entry(ChangeKind::node_removed, id);
    change.parent = node.parent;
    change.index = child_index(before, id).value_or(no_index);
    change.role = node.role;
    out.push_back(std::move(change));
  }
  for (const auto &[id, node] : after.nodes) {
    if (before.nodes.contains(id)) continue;
    Change change = entry(ChangeKind::node_added, id);
    change.parent = node.parent;
    change.index = child_index(after, id).value_or(no_index);
    change.role = node.role;
    out.push_back(std::move(change));
  }
  for (const NodeId id : reading_order(after)) {
    const auto prior = before.nodes.find(id);
    const auto node = after.nodes.find(id);
    if (prior == before.nodes.end() || node == after.nodes.end()) continue;
    const auto &was = prior->second.children;
    const auto &now = node->second.children;
    const auto old_index = [&was](const NodeId child) -> std::size_t {
      const auto found = std::find(was.begin(), was.end(), child);
      return found == was.end() ? no_index
                                : static_cast<std::size_t>(found - was.begin());
    };
    const auto new_index = [&now](const NodeId child) -> std::size_t {
      const auto found = std::find(now.begin(), now.end(), child);
      return found == now.end() ? no_index
                                : static_cast<std::size_t>(found - now.begin());
    };
    const auto child = [&out, id](const ChangeKind kind, const NodeId moved,
                                  const std::size_t index) {
      Change change = entry(kind, moved);
      change.parent = id;
      change.index = index;
      out.push_back(std::move(change));
    };
    // Removals first: vanished children, then surviving children that moved.
    for (std::size_t index = 0; index < was.size(); ++index) {
      if (new_index(was[index]) == no_index)
        child(ChangeKind::child_removed, was[index], index);
    }
    for (std::size_t index = 0; index < was.size(); ++index) {
      const auto moved = new_index(was[index]);
      if (moved != no_index && moved != index)
        child(ChangeKind::child_removed, was[index], index);
    }
    // Then additions: new children, then the moved children at their new index.
    for (std::size_t index = 0; index < now.size(); ++index) {
      if (old_index(now[index]) == no_index)
        child(ChangeKind::child_added, now[index], index);
    }
    for (std::size_t index = 0; index < now.size(); ++index) {
      const auto moved = old_index(now[index]);
      if (moved != no_index && moved != index)
        child(ChangeKind::child_added, now[index], index);
    }
  }
}

constexpr bool continuation(const char byte) noexcept {
  return (static_cast<unsigned char>(byte) & 0xc0U) == 0x80U;
}

struct Edit {
  std::size_t offset{};
  std::string removed, inserted;
};

// Common prefix and suffix, snapped back so neither side splits a code point.
std::optional<Edit> text_edit(const std::string_view before,
                              const std::string_view after) {
  if (!layerc::core::is_valid_utf8(before) ||
      !layerc::core::is_valid_utf8(after))
    return std::nullopt;
  const std::size_t shared = std::min(before.size(), after.size());
  std::size_t prefix = 0;
  while (prefix < shared && before[prefix] == after[prefix]) ++prefix;
  std::size_t suffix = 0;
  while (suffix < shared - prefix &&
         before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix])
    ++suffix;
  while (prefix > 0 && prefix < before.size() && prefix < after.size() &&
         (continuation(before[prefix]) || continuation(after[prefix])))
    --prefix;
  while (suffix > 0 && (continuation(before[before.size() - suffix]) ||
                        continuation(after[after.size() - suffix])))
    --suffix;
  Edit edit;
  edit.offset = prefix;
  edit.removed.assign(before.substr(prefix, before.size() - prefix - suffix));
  edit.inserted.assign(after.substr(prefix, after.size() - prefix - suffix));
  if (edit.removed.empty() && edit.inserted.empty()) return std::nullopt;
  return edit;
}

void properties(const Node &before, const Node &after, std::vector<Change> &out) {
  if (before.name != after.name) {
    Change change = entry(ChangeKind::name_changed, after.id);
    change.previous = before.name;
    change.text = after.name;
    out.push_back(std::move(change));
  }
  if (before.description != after.description) {
    Change change = entry(ChangeKind::description_changed, after.id);
    change.previous = before.description;
    change.text = after.description;
    out.push_back(std::move(change));
  }
  if (before.value != after.value) {
    Change change = entry(ChangeKind::value_changed, after.id);
    change.previous = before.value;
    change.text = after.value;
    out.push_back(std::move(change));
  } else if (before.text && after.text &&
             before.text->value != after.text->value) {
    Change change = entry(ChangeKind::value_changed, after.id);
    change.previous = before.text->value;
    change.text = after.text->value;
    change.from_text = true;
    out.push_back(std::move(change));
  }
}

void states(const Node &before, const Node &after, std::vector<Change> &out) {
  for (const StateKey key : all_states) {
    const bool was = flag(before.states, key);
    const bool now = flag(after.states, key);
    if (was == now) continue;
    Change change = entry(ChangeKind::state_changed, after.id);
    change.state = key;
    change.state_value = now;
    out.push_back(std::move(change));
  }
}

void text(const Node &before, const Node &after, std::vector<Change> &out) {
  if (!before.text || !after.text) return;
  if (const auto edit = text_edit(before.text->value, after.text->value)) {
    if (!edit->removed.empty()) {
      Change change = entry(ChangeKind::text_deleted, after.id);
      change.offset = edit->offset;
      change.previous = edit->removed;
      out.push_back(std::move(change));
    }
    if (!edit->inserted.empty()) {
      Change change = entry(ChangeKind::text_inserted, after.id);
      change.offset = edit->offset;
      change.text = edit->inserted;
      out.push_back(std::move(change));
    }
  }
  if (before.text->caret != after.text->caret) {
    Change change = entry(ChangeKind::caret_moved, after.id);
    change.offset = after.text->caret;
    out.push_back(std::move(change));
  }
  if (before.text->anchor != after.text->anchor ||
      before.text->caret != after.text->caret) {
    Change change = entry(ChangeKind::selection_changed, after.id);
    change.anchor = after.text->anchor;
    change.caret = after.text->caret;
    out.push_back(std::move(change));
  }
}

void focus(const Snapshot &before, const Snapshot &after,
           std::vector<Change> &out) {
  NodeId winner = no_node;
  for (const NodeId id : reading_order(after)) {
    const auto node = after.nodes.find(id);
    if (node == after.nodes.end() || !node->second.states.focused) continue;
    const auto prior = before.nodes.find(id);
    if (prior != before.nodes.end() && prior->second.states.focused) continue;
    winner = id;
  }
  if (winner != no_node) out.push_back(entry(ChangeKind::focus_moved, winner));
}

bool switchable(const Node &node) {
  return node.role == Role::window || node.role == Role::dialog;
}

void windows(const Snapshot &before, const Snapshot &after,
             std::vector<Change> &out) {
  for (const NodeId id : reading_order(after)) {
    const auto prior = before.nodes.find(id);
    const auto node = after.nodes.find(id);
    if (prior == before.nodes.end() || node == after.nodes.end()) continue;
    if (!switchable(node->second)) continue;
    if (!prior->second.states.active && node->second.states.active)
      out.push_back(entry(ChangeKind::window_activated, id));
    else if (prior->second.states.active && !node->second.states.active)
      out.push_back(entry(ChangeKind::window_deactivated, id));
  }
  for (const auto &[id, node] : before.nodes) {
    if (after.nodes.contains(id) || !switchable(node) || !node.states.active)
      continue;
    out.push_back(entry(ChangeKind::window_deactivated, id));
  }
}

void announcement(const Snapshot &before, const Snapshot &after,
                  std::vector<Change> &out) {
  if (after.announcement.empty() ||
      after.announcement_sequence == before.announcement_sequence)
    return;
  Change change = entry(ChangeKind::announced, after.announcement_source);
  change.text = after.announcement;
  out.push_back(std::move(change));
}

} // namespace

std::vector<Change> changes(const Snapshot &before, const Snapshot &after) {
  std::vector<Change> out;
  structure(before, after, out);
  // A first tree is additions only: every node is new, so no property, state,
  // text, focus, window or announcement difference describes existing content.
  if (before.nodes.empty()) return out;
  for (const NodeId id : reading_order(after)) {
    const auto prior = before.nodes.find(id);
    const auto node = after.nodes.find(id);
    if (prior == before.nodes.end() || node == after.nodes.end()) continue;
    properties(prior->second, node->second, out);
    states(prior->second, node->second, out);
    text(prior->second, node->second, out);
  }
  focus(before, after, out);
  windows(before, after, out);
  announcement(before, after, out);
  return out;
}

} // namespace layerc::editor::a11y
