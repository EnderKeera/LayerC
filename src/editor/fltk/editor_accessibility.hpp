#pragma once

#include "a11y/atspi/adaptor.hpp"

#include <functional>
#include <memory>
#include <string>

class Fl_Widget;

namespace layerc::editor {
class EditorApp;

// The one wording of the visible accessibility line, so the status pane and the
// tests that check it cannot drift apart.
[[nodiscard]] std::string
accessibility_status_line(const a11y::atspi::AdaptorStatus &status);

// Publishes LayerE's FLTK widgets, including modal dialogs, on the
// accessibility bus through the editor's own AT-SPI adaptor. FLTK owns the UI
// loop: every message is handled on it, so there is no thread, no polling timer
// and no synthetic input. While no assistive technology listens nothing is
// exported, no snapshot is taken and no bus launcher is started.
class EditorAccessibility final {
public:
  explicit EditorAccessibility(EditorApp &app);
  ~EditorAccessibility();
  EditorAccessibility(const EditorAccessibility &) = delete;
  EditorAccessibility &operator=(const EditorAccessibility &) = delete;
  [[nodiscard]] a11y::atspi::AdaptorStatus status() const;
  // Reports every later change on the FLTK thread and delivers the current
  // state once, so a late caller can render the initial line from it.
  using StatusHandler = std::function<void(const a11y::atspi::AdaptorStatus &)>;
  void set_status_handler(StatusHandler handler);
  // Speaks `text` as an AT-SPI announcement (DesignSystem.md, Screen readers).
  // Does nothing while no consumer listens; never queues on a thread.
  void announce(Fl_Widget *source, const std::string &text);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
