// Detected Tasks on Windows: the attachable top-level windows the Win32
// editor lists, read through the shared window query. Reads only.
#include "platform/services.hpp"

#include "layerc/core/game_match.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/window_query.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

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
