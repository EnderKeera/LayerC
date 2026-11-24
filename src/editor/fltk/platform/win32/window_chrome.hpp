#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

class Fl_Window;

namespace layerc::editor {

class WindowsWindowDecorator final {
public:
  using SetAffinity = bool (*)(HWND, bool);
  using ReadAffinity = decltype(&GetWindowDisplayAffinity);
  using SetFrameColors = void (*)(HWND, COLORREF, COLORREF, COLORREF);
  using WarningReporter = std::function<void(std::string_view)>;

  WindowsWindowDecorator();
  explicit WindowsWindowDecorator(SetAffinity set_affinity,
                                  ReadAffinity read_affinity = GetWindowDisplayAffinity,
                                  SetFrameColors set_frame_colors = nullptr);
  ~WindowsWindowDecorator();
  WindowsWindowDecorator(const WindowsWindowDecorator &) = delete;
  WindowsWindowDecorator &operator=(const WindowsWindowDecorator &) = delete;

  void refresh();
  [[nodiscard]] bool verify(Fl_Window &window);
  void adopt_for_test(HWND root, bool failed, bool token_matches);
  void set_warning_reporter(WarningReporter report);
  [[nodiscard]] std::string_view capture_warning() const noexcept { return warning_; }
  [[nodiscard]] std::size_t decorated_window_count() const noexcept { return decorated_.size(); }

private:
  enum class Affinity { untouched, verified, failed };
  struct WindowState {
    UINT_PTR token{};
    std::uint64_t caption_generation{};
    Affinity affinity{Affinity::untouched};
  };

  static int before_paint(void *event, void *data);
  static void before_wait(void *data);
  [[nodiscard]] WindowState &find_or_enroll(HWND root, bool recheck_affinity);
  [[nodiscard]] WindowState &enroll(HWND root);
  void decorate(HWND root, WindowState &state);
  void reconcile_affinity(HWND root, WindowState &state);
  void update_warning();
  void prune();

  const SetAffinity set_affinity_;
  const ReadAffinity read_affinity_;
  const SetFrameColors set_frame_colors_;
  const ATOM lifetime_atom_;
  WarningReporter report_;
  std::string_view warning_;
  std::uint64_t generation_{1};
  std::unordered_map<HWND, WindowState> decorated_;
};

} // namespace layerc::editor
