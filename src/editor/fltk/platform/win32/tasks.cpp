// Detected Tasks on Windows: the attachable top-level windows the Win32
// editor lists, read through the shared window query. Reads only.
#include "platform/services.hpp"

#include "layerc/core/game_match.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/window_query.hpp"

#include <windows.h>
#include <commctrl.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace layerc::editor {
namespace {

// Matches the Linux scan's bound in spirit: a desktop rarely has more, and
// the list stays responsive when it does.
constexpr std::size_t scan_limit = 512;

std::string utf8(const std::wstring &text) {
  return core::try_wide_to_utf8(text).value_or(std::string{});
}

TaskCandidate candidate(const platform::WindowCandidate &window, const HWND active) {
  TaskCandidate task;
  task.window = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window.window));
  task.title = utf8(window.title);
  task.class_name = utf8(window.window_class);
  task.executable_name = utf8(window.executable_name);
  task.executable_path = utf8(window.executable_path);
  if (window.process_id != 0) task.pid = static_cast<std::uint32_t>(window.process_id);
  RECT client{};
  if (GetClientRect(window.window, &client)) {
    task.width = static_cast<int>(client.right - client.left);
    task.height = static_cast<int>(client.bottom - client.top);
  }
  task.active = window.window == active;
  task.hidden = window.background;
  return task;
}

class WindowsPickCaptureWatch final : public PickCaptureWatch {
public:
  WindowsPickCaptureWatch(PickCaptureTimer timer, std::function<void()> lost)
      : timer_(timer), lost_(std::move(lost)) {}

  ~WindowsPickCaptureWatch() override {
    detach();
    if (pending_) timer_.cancel(notify, this);
  }

  [[nodiscard]] bool attach(HWND owner) {
    if (!owner || !SetWindowSubclass(owner, subclass, id(), reinterpret_cast<DWORD_PTR>(this)))
      return false;
    owner_ = owner;
    return true;
  }

private:
  [[nodiscard]] UINT_PTR id() const noexcept { return reinterpret_cast<UINT_PTR>(this); }

  void detach() noexcept {
    if (owner_) RemoveWindowSubclass(std::exchange(owner_, nullptr), subclass, id());
  }

  static LRESULT CALLBACK subclass(HWND owner, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR, DWORD_PTR data) {
    auto &watch = *reinterpret_cast<WindowsPickCaptureWatch *>(data);
    if (message == WM_NCDESTROY) watch.detach();
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lparam) != owner &&
        !watch.pending_) {
      watch.pending_ = true;
      watch.timer_.schedule(0.0, notify, &watch);
    }
    return DefSubclassProc(owner, message, wparam, lparam);
  }

  static void notify(void *data) {
    auto &watch = *static_cast<WindowsPickCaptureWatch *>(data);
    watch.pending_ = false;
    auto lost = std::move(watch.lost_);
    if (lost) lost();
  }

  HWND owner_{};
  PickCaptureTimer timer_;
  std::function<void()> lost_;
  bool pending_{};
};

} // namespace

std::vector<TaskCandidate> scan_tasks(std::string &error) {
  error.clear();
  const auto windows = platform::enumerate_attachable_windows();
  const HWND active = GetForegroundWindow();
  std::vector<TaskCandidate> tasks;
  tasks.reserve(windows.size() < scan_limit ? windows.size() : scan_limit);
  for (const auto &window : windows) {
    if (tasks.size() == scan_limit) {
      error = "Only the first " + std::to_string(scan_limit) + " windows are listed.";
      break;
    }
    tasks.push_back(candidate(window, active));
  }
  return tasks;
}

bool window_picking_supported() noexcept { return true; }

std::optional<TaskCandidate> task_at_screen_point(const int x, const int y) {
  const POINT point{x, y};
  if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL)) return std::nullopt;
  const auto window = platform::window_at_point(point);
  if (!window) return std::nullopt;
  return candidate(*window, GetForegroundWindow());
}

std::optional<TaskCandidate> task_under_cursor() {
  POINT point{};
  if (!GetCursorPos(&point)) return std::nullopt;
  return task_at_screen_point(static_cast<int>(point.x), static_cast<int>(point.y));
}

std::unique_ptr<PickCaptureWatch>
watch_pick_capture(std::uintptr_t owner, PickCaptureTimer timer, std::function<void()> lost) {
  auto watch = std::make_unique<WindowsPickCaptureWatch>(timer, std::move(lost));
  if (!watch->attach(reinterpret_cast<HWND>(owner))) return nullptr;
  return watch;
}

std::optional<TaskCandidate> matching_task(const std::vector<TaskCandidate> &tasks,
                                           const core::GameMatch &match) {
  // Windows filesystems compare case-insensitively, as LayerD matches them.
  std::vector<core::MatchCandidate> candidates;
  std::vector<std::size_t> origin;
  candidates.reserve(tasks.size());
  origin.reserve(tasks.size());
  for (std::size_t index{}; index < tasks.size(); ++index) {
    const auto &task = tasks[index];
    if (task.pid && *task.pid == current_process_id()) continue;
    candidates.push_back({.executable_name = task.executable_name,
                          .executable_path = task.executable_path,
                          .class_name = task.class_name,
                          .title = task.title,
                          .active = task.active});
    origin.push_back(index);
  }
  const auto resolved =
      core::resolve_game_match(candidates, match, core::GameMatchSelection{});
  if (!resolved) return std::nullopt;
  return tasks[origin[*resolved]];
}

bool task_matches(const TaskCandidate &task, const core::GameMatch &match) {
  return matching_task({task}, match).has_value();
}

} // namespace layerc::editor
