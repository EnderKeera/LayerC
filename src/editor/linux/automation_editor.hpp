#pragma once

#include <memory>
#include <optional>
#include <string>

class Fl_Group;

namespace layerc::editor {
class EditorApp;

// The Linux controls for the shared graph and deterministic simulator. FLTK
// owns the widgets; this object owns their callbacks and playback lifetime.
class LinuxAutomationEditor final {
public:
  explicit LinuxAutomationEditor(EditorApp &app);
  ~LinuxAutomationEditor();
  LinuxAutomationEditor(const LinuxAutomationEditor &) = delete;
  LinuxAutomationEditor &operator=(const LinuxAutomationEditor &) = delete;
  [[nodiscard]] Fl_Group *group() const;
  void refresh();
  // Returns the failure text the caller must report; the validation box and
  // selection/focus side effects stay inside the editor.
  [[nodiscard]] std::optional<std::string> apply_pending();
  void stop();
private:
  struct Impl;
  EditorApp &app_;
  Fl_Group *page_{};
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
