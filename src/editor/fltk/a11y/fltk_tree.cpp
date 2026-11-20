#include "fltk_tree.hpp"

#include "input.hpp"
#include "text_metrics.hpp"
#include "window_geometry.hpp"

#include "layerc/core/utf8.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Valuator.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>

#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace layerc::editor::a11y {
namespace {

// Target as an identity: one widget, one browser row, one inline menu item.
struct Key {
  Fl_Widget *widget{};
  TargetKind kind{TargetKind::widget};
  int index{};
  [[nodiscard]] bool operator<(const Key &other) const noexcept {
    if (widget != other.widget) return std::less<Fl_Widget *>{}(widget, other.widget);
    if (kind != other.kind) return kind < other.kind;
    return index < other.index;
  }
};

struct Entry {
  Key key;
  NodeId id{};
  NodeId parent{};
  // Created while the widget was alive and kept until the entry retires: a
  // tracker made later cannot report that an older pointer was already gone.
  std::unique_ptr<Fl_Widget_Tracker> tracker;
};

// A secret field exports no text. Text whose bytes or offsets the model would
// reject exports no text interface either, so one field cannot fail the whole
// snapshot; its name, role and states still export.
[[nodiscard]] std::optional<Text> exportable(const States &states, std::optional<Text> text) {
  if (!text || states.password) return std::nullopt;
  const std::string_view value = text->value;
  if (!core::is_valid_utf8(value) || text->caret > value.size() || text->anchor > value.size() ||
      !core::is_valid_utf8(value.substr(0, text->caret)) ||
      !core::is_valid_utf8(value.substr(0, text->anchor)))
    return std::nullopt;
  return text;
}
} // namespace

struct FltkTree::Impl {
  std::map<Key, std::unique_ptr<Entry>> live;
  std::map<NodeId, Key> by_id;
  std::map<const Fl_Widget *, NodeId> widgets;
  struct ScreenSpace {
    std::optional<std::pair<int, int>> origin;
    double scale{};
  };
  // A single native coordinate query per window and pass, not per list row
  // or text glyph. Unavailable origins are cached for the pass as well.
  std::map<Fl_Window *, ScreenSpace> spaces;
  // Last pass in traversal order; FLTK reorders its window list on activation,
  // so the previous pass's order keeps surviving children in place.
  std::vector<std::pair<Target, NodeId>> previous;
  // Advances already measured for the texts this tree exports; a pass drops
  // whatever it did not use, so only the current fields stay cached.
  TextMetrics metrics;
  NodeId root{};
  NodeId next_id{1};

  [[nodiscard]] NodeId allocate() { return next_id++; }

  [[nodiscard]] NodeId root_id() {
    if (root != no_node) return root;
    root = allocate();
    auto entry = std::make_unique<Entry>();
    entry->key = {nullptr, TargetKind::application, 0};
    entry->id = root;
    by_id[root] = entry->key;
    live[entry->key] = std::move(entry);
    return root;
  }

  // Reuses a surviving target's id, or allocates one that no retired target
  // ever had. A live widget that reuses a dead target's address is a new
  // target: its tracker belongs to the deleted widget and cannot vouch for it.
  [[nodiscard]] NodeId slot(const Target &target, NodeId parent) {
    const Key key{target.widget, target.kind, target.index};
    const auto found = live.find(key);
    if (found != live.end()) {
      const bool alive = key.kind == TargetKind::application ||
                         (found->second->tracker && found->second->tracker->widget() != nullptr);
      if (alive) {
        found->second->parent = parent;
        return found->second->id;
      }
      by_id.erase(found->second->id);
      if (key.kind == TargetKind::widget) widgets.erase(key.widget);
      live.erase(found);
    }
    auto entry = std::make_unique<Entry>();
    entry->key = key;
    entry->id = allocate();
    entry->parent = parent;
    if (target.widget != nullptr) entry->tracker = std::make_unique<Fl_Widget_Tracker>(target.widget);
    const auto id = entry->id;
    by_id[id] = key;
    if (key.kind == TargetKind::widget && key.widget != nullptr) widgets[key.widget] = id;
    live[std::move(key)] = std::move(entry);
    return id;
  }

  [[nodiscard]] std::optional<Bounds> screen_bounds(const Target &target,
                                                   const Bounds &local) {
    auto *window = target.widget != nullptr ? target.widget->top_window() : nullptr;
    if (window == nullptr) return std::nullopt;
    auto [space, inserted] = spaces.try_emplace(window);
    if (inserted) {
      space->second.origin = window_screen_origin(*window);
      space->second.scale = static_cast<double>(Fl::screen_scale(window->screen_num()));
    }
    const auto &origin = space->second.origin;
    const double scale = space->second.scale;
    if (!origin) return std::nullopt;
    const auto physical = [scale](double value) { return std::round(value * scale); };
    return Bounds{origin->first + physical(local.left), origin->second + physical(local.top),
                  physical(local.width), physical(local.height)};
  }

  [[nodiscard]] NodeId visit(const Target &target, NodeId parent, Snapshot &snapshot,
                             std::vector<std::pair<Target, NodeId>> &next,
                             const std::map<NodeId, std::vector<Target>> &previous_children) {
    const auto id = slot(target, parent);
    next.emplace_back(target, parent);

    Node node;
    node.id = id;
    node.parent = parent;
    node.role = role_of(target);
    node.name = name(target);
    node.description = description(target);
    node.states = states(target);
    node.keybinding = keybinding(target);
    node.actions = action_names(target);
    if (target.kind == TargetKind::widget) {
      if (auto *field = dynamic_cast<Fl_Input_ *>(target.widget))
        node.text = exportable(node.states, field_text(*field, &metrics));
      if (const auto *choice = dynamic_cast<const Fl_Choice *>(target.widget)) {
        // A non-editable combo exposes its committed label, not popup focus.
        node.value = valid_utf8(clean_label(choice->text()));
        node.states.read_only = true;
      }
      if (auto *valuator = dynamic_cast<Fl_Valuator *>(target.widget)) node.range = range_of(*valuator);
      node.scroll = scroll_of(target);
    }
    if (const auto local = local_bounds(target))
      if (const auto bounds = screen_bounds(target, *local)) node.bounds = *bounds;
    if (node.text) {
      node.value = node.text->value;
      node.text->caret_bounds = screen_bounds(target, node.text->caret_bounds).value_or(Bounds{});
      for (auto &run : node.text->runs)
        run.bounds = screen_bounds(target, run.bounds).value_or(Bounds{});
    }

    std::vector<Target> previous_list;
    if (const auto found = previous_children.find(id); found != previous_children.end())
      previous_list = found->second;
    for (const auto &child : desired_children(target, previous_list))
      node.children.push_back(visit(child, id, snapshot, next, previous_children));
    snapshot.nodes[id] = std::move(node);
    return id;
  }

  // A target this pass did not export is retired for good: a hidden page's
  // rows vanish, and their ids stay retired when the page returns.
  void finish(const std::vector<std::pair<Target, NodeId>> &next) {
    std::map<Key, std::size_t> kept;
    for (const auto &[target, parent] : next) kept[{target.widget, target.kind, target.index}] = 1;
    for (auto it = live.begin(); it != live.end();) {
      if (kept.contains(it->first)) {
        ++it;
        continue;
      }
      by_id.erase(it->second->id);
      if (it->first.kind == TargetKind::widget) widgets.erase(it->first.widget);
      it = live.erase(it);
    }
    previous = next;
  }
};

FltkTree::FltkTree() : impl_(std::make_unique<Impl>()) {}
FltkTree::~FltkTree() = default;

Snapshot FltkTree::snapshot() {
  // One pass: every menu's size is walked once, not once per question, and
  // every exported field's text geometry once per glyph.
  TreePass pass;
  TextMetrics::Pass measure(impl_->metrics);
  impl_->spaces.clear();
  Snapshot snapshot;
  snapshot.root = impl_->root_id();
  std::map<NodeId, std::vector<Target>> previous_children;
  for (const auto &[target, parent] : impl_->previous) previous_children[parent].push_back(target);
  std::vector<std::pair<Target, NodeId>> next;
  next.reserve(impl_->previous.size() + 16);
  (void)impl_->visit({nullptr, TargetKind::application, 0}, no_node, snapshot, next,
                     previous_children);
  impl_->finish(next);
  return snapshot;
}

std::optional<Target> FltkTree::target(const NodeId id) const {
  const auto found = impl_->by_id.find(id);
  if (found == impl_->by_id.end()) return std::nullopt;
  const auto entry = impl_->live.find(found->second);
  if (entry == impl_->live.end()) return std::nullopt;
  if (found->second.kind != TargetKind::application &&
      (entry->second->tracker == nullptr || entry->second->tracker->widget() == nullptr))
    return std::nullopt;
  return Target{found->second.widget, found->second.kind, found->second.index};
}

NodeId FltkTree::id_for(const Fl_Widget &widget) const {
  const auto found = impl_->widgets.find(&widget);
  return found == impl_->widgets.end() || !target(found->second) ? no_node : found->second;
}

void FltkTree::clear() {
  impl_->live.clear();
  impl_->by_id.clear();
  impl_->widgets.clear();
  impl_->previous.clear();
  impl_->spaces.clear();
  impl_->metrics = TextMetrics{};
  impl_->root = no_node;
}
} // namespace layerc::editor::a11y
