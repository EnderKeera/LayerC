#pragma once

#include <memory>

class Fl_Group;

namespace layerc::editor {
class EditorApp;
class LinuxVisualEditor final {
public:
  explicit LinuxVisualEditor(EditorApp &app);
  ~LinuxVisualEditor();
  LinuxVisualEditor(const LinuxVisualEditor &) = delete;
  LinuxVisualEditor &operator=(const LinuxVisualEditor &) = delete;
  [[nodiscard]] Fl_Group *group() const;
  void refresh();
private:
  struct Impl;
  EditorApp &app_;
  Fl_Group *page_{};
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
