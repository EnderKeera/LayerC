#pragma once

#include "accessibility_semantics.hpp"

namespace layerc::editor::accessibility {

class AccessibilityTree final {
public:
  AccessibilityTree() = default;
  ~AccessibilityTree();
  AccessibilityTree(const AccessibilityTree &) = delete;
  AccessibilityTree &operator=(const AccessibilityTree &) = delete;
  bool initialize(std::string &fault);
  bool owns_root() const noexcept { return root_ != nullptr; }
  void reconcile(bool quiet);
  void sync();
  // Raises the announcement on `source`'s object, else on the active window's,
  // else on the root (DesignSystem.md, Screen readers). Call after sync().
  void announce(Fl_Widget *source, const std::string &text);
  // Detach with all references alive, then release after adaptor cleanup.
  void detach();
  void release();

private:
  static void collect(AtkObject *object, std::vector<AtkObject *> &objects);
  // The object that stands for `widget`, or null when it is not exported.
  static AtkObject *find(AtkObject *object, const Fl_Widget *widget);
  void retire(AtkObject *object, bool quiet);
  void reconcile(AtkObject *object, bool quiet);
  void report_changes(AtkObject *object);
  static void report_text(AtkObject *object, Node &node, const std::string &text);
  void report_window();
  AtkObject *root_{};
  AtkObject *active_window_{};
  std::vector<AtkObject *> detached_;
};

} // namespace layerc::editor::accessibility
