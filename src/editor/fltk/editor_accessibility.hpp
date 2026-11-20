#pragma once

#include "accessibility_status.hpp"

#include <functional>
#include <memory>
#include <string>

class Fl_Widget;

namespace layerc::editor {
class EditorApp;

// The one wording of the visible accessibility line, so the status pane and the
// tests that check it cannot drift apart.
// Publishes LayerE's FLTK widgets, including modal dialogs, through the
// platform accessibility bridge. FLTK owns the UI loop: every message is
// handled on it, so there is no thread, no polling timer and no synthetic input.
class EditorAccessibility final {
public:
  explicit EditorAccessibility(EditorApp &app);
  ~EditorAccessibility();
  EditorAccessibility(const EditorAccessibility &) = delete;
  EditorAccessibility &operator=(const EditorAccessibility &) = delete;
  [[nodiscard]] AccessibilityStatus status() const;
  // Reports status on the FLTK thread and delivers the current state once.
  using StatusHandler = std::function<void(const AccessibilityStatus &)>;
  void set_status_handler(StatusHandler handler);
  // Speaks `text` through the platform accessibility bridge. Never queues on a thread.
  void announce(Fl_Widget *source, const std::string &text);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
