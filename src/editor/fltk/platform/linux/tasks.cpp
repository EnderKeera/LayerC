#include "platform/services.hpp"
#include "x11_window.hpp"

namespace layerc::editor {
namespace {
constexpr std::uint32_t scan_limit = 64;

TaskCandidate candidate(const platform::X11WindowSnapshot &window) {
  return {window.window, window.title, window.class_name, window.executable_name,
          window.executable_path, window.pid, window.width, window.height, window.active,
          window.hidden};
}

platform::X11WindowSnapshot snapshot(const TaskCandidate &task) {
  platform::X11WindowSnapshot window;
  window.window = task.window;
  window.title = task.title;
  window.class_name = task.class_name;
  window.executable_name = task.executable_name;
  window.executable_path = task.executable_path;
  window.pid = task.pid;
  window.width = task.width;
  window.height = task.height;
  window.active = task.active;
  window.hidden = task.hidden;
  return window;
}
} // namespace

std::vector<TaskCandidate> scan_tasks(std::string &error) {
  error.clear();
  auto display = platform::X11Display::open(error);
  if (!display) {
    if (error.empty()) error = "the X11 display could not be opened";
    return {};
  }
  const platform::X11WindowTracker tracker(*display);
  const auto scan = tracker.scan(scan_limit);
  if (!scan.warning.empty()) error = scan.warning;
  std::vector<TaskCandidate> tasks;
  tasks.reserve(scan.windows.size());
  for (const auto &window : scan.windows) tasks.push_back(candidate(window));
  return tasks;
}

std::optional<TaskCandidate> matching_task(const std::vector<TaskCandidate> &tasks,
                                           const core::GameMatch &match) {
  platform::X11WindowScan scan;
  scan.authoritative = true;
  for (const auto &task : tasks)
    if (!task.pid || *task.pid != current_process_id())
      scan.windows.push_back(snapshot(task));
  const auto *found = platform::match_window(scan, match);
  if (found == nullptr) return std::nullopt;
  return candidate(*found);
}

bool task_matches(const TaskCandidate &task, const core::GameMatch &match) {
  return matching_task({task}, match).has_value();
}

} // namespace layerc::editor
