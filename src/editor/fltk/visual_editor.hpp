#pragma once

#include <memory>

class Fl_Group;

namespace layerc::editor {
class EditorApp;
struct TaskCandidate;
class LinuxVisualEditor final {
public:
  explicit LinuxVisualEditor(EditorApp &app);
  ~LinuxVisualEditor();
  LinuxVisualEditor(const LinuxVisualEditor &) = delete;
  LinuxVisualEditor &operator=(const LinuxVisualEditor &) = delete;
  [[nodiscard]] Fl_Group *group() const;
  void refresh();
  void calibrate_task(const TaskCandidate &task);
private:
  void ensure_impl();
  struct Impl;
  EditorApp &app_;
  Fl_Group *page_{};
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
