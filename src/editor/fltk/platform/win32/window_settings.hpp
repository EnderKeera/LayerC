#pragma once

#include "status_kind.hpp"

#include <functional>
#include <string>

namespace layerc::editor {

void multiply_windows_screen_scales(float ratio, int screen = -1);
void set_windows_desktop_text_scale(float ratio);
void restore_windows_screen_text_scale(int screen);
void load_startup_text_scale();
void set_editor_capture_exclusion(bool enabled);

class WindowsTextScaleShortcuts final {
public:
  using Reporter = std::function<void(StatusKind, std::string)>;
  explicit WindowsTextScaleShortcuts(Reporter report);
  ~WindowsTextScaleShortcuts();

  WindowsTextScaleShortcuts(const WindowsTextScaleShortcuts &) = delete;
  WindowsTextScaleShortcuts &operator=(const WindowsTextScaleShortcuts &) = delete;

private:
  Reporter report_;
};

} // namespace layerc::editor
