#pragma once

#include <memory>
#include <string>

class Fl_Widget;

namespace layerc::editor {
class EditorApp;

// Exports LayerE's FLTK widgets, including modal dialogs, through ATK and its
// maintained AT-SPI adaptor without GTK widgets. FLTK owns the UI loop: GLib
// readiness is integrated into it, so there is no thread, polling timer or
// synthetic input. Without a usable adaptor or accessibility bus the editor
// keeps working and fault() names the visible reason.
class EditorAccessibility final {
public:
  explicit EditorAccessibility(EditorApp &app);
  ~EditorAccessibility();
  EditorAccessibility(const EditorAccessibility &) = delete;
  EditorAccessibility &operator=(const EditorAccessibility &) = delete;
  [[nodiscard]] bool available() const;
  [[nodiscard]] const std::string &fault() const;
  // Speaks `text` as an AT-SPI announcement (DesignSystem.md, Screen readers)
  // after syncing the tree so a just-written status line is known. Does
  // nothing while the bridge is unavailable; never queues on a thread.
  void announce(Fl_Widget *source, const std::string &text);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
