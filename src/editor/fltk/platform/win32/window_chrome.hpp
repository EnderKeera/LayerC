#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace layerc::editor {

class WindowsWindowDecorator final {
public:
  using SetAffinity = bool (*)(HWND, bool);
  using WarningReporter = std::function<void(std::string_view)>;

  WindowsWindowDecorator();
  explicit WindowsWindowDecorator(SetAffinity set_affinity);
  ~WindowsWindowDecorator();
  WindowsWindowDecorator(const WindowsWindowDecorator &) = delete;
  WindowsWindowDecorator &operator=(const WindowsWindowDecorator &) = delete;

  void refresh();
  void set_warning_reporter(WarningReporter report);
  [[nodiscard]] std::string_view capture_warning() const noexcept { return warning_; }
  [[nodiscard]] std::size_t decorated_window_count() const noexcept { return decorated_.size(); }

private:
  enum class Affinity { untouched, verified, failed };
  struct WindowState {
    std::uint64_t caption_generation{};
    Affinity affinity{Affinity::untouched};
  };

  static int before_paint(void *event, void *data);
  static void before_wait(void *data);
  void decorate(HWND root, bool recheck_affinity);
  void reconcile_affinity(HWND root, WindowState &state);
  void update_warning();
  void prune();

  const SetAffinity set_affinity_;
  WarningReporter report_;
  std::string_view warning_;
  std::uint64_t generation_{1};
  std::unordered_map<HWND, WindowState> decorated_;
};

} // namespace layerc::editor
