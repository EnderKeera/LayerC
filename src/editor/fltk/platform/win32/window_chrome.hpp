#pragma once

#include <windows.h>

#include <cstdint>
#include <unordered_map>

namespace layerc::editor {

class WindowsWindowDecorator final {
public:
  WindowsWindowDecorator();
  ~WindowsWindowDecorator();
  WindowsWindowDecorator(const WindowsWindowDecorator &) = delete;
  WindowsWindowDecorator &operator=(const WindowsWindowDecorator &) = delete;

  void refresh();
  void clear_capture_exclusion();
  [[nodiscard]] std::size_t decorated_window_count() const noexcept { return decorated_.size(); }

private:
  static int before_paint(void *event, void *data);
  void decorate(HWND root);
  void prune();

  std::uint64_t generation_{1};
  std::unordered_map<HWND, std::uint64_t> decorated_;
};

} // namespace layerc::editor
