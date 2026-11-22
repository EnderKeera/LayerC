#pragma once

#include <windows.h>

#include <functional>

namespace layerc::editor {

class WindowsDesktopListener final {
public:
  WindowsDesktopListener(HINSTANCE instance, std::function<void()> reapply);
  ~WindowsDesktopListener();
  WindowsDesktopListener(const WindowsDesktopListener &) = delete;
  WindowsDesktopListener &operator=(const WindowsDesktopListener &) = delete;

  [[nodiscard]] HWND native_handle() const noexcept { return window_; }

private:
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  static void refresh(void *data);

  HINSTANCE instance_;
  ATOM window_class_{};
  HWND window_{};
  std::function<void()> reapply_;
  bool pending_{};
};

} // namespace layerc::editor
