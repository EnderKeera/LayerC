#pragma once

#include "fltk_semantics.hpp"
#include "model.hpp"

#include <memory>
#include <optional>

class Fl_Widget;

namespace layerc::editor::a11y {

// Exports the live FLTK widget tree as the toolkit-free model: one node per
// widget, browser row and inline menu item, in reading order. Toolkit only:
// the FLTK thread calls snapshot(), an adaptor publishes the result into Model,
// and the tree never publishes itself. Ids are stable while a target survives
// and are never reused, including after clear().
class FltkTree {
public:
  FltkTree();
  ~FltkTree();
  FltkTree(const FltkTree &) = delete;
  FltkTree &operator=(const FltkTree &) = delete;

  [[nodiscard]] Snapshot snapshot();
  // The target a snapshot's node id named, or nothing once its widget was
  // deleted or the target left the tree.
  [[nodiscard]] std::optional<Target> target(NodeId id) const;
  // The node id a widget's own node has now, or no_node while it has none.
  [[nodiscard]] NodeId id_for(const Fl_Widget &widget) const;
  // Forgets every id as if the widgets were new; the next snapshot continues
  // the id sequence, so a forgotten id never names a later node.
  void clear();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::editor::a11y
