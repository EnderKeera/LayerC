#include "task_list.hpp"

#include "layerc/core/utf8.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace layerc::editor {

std::vector<TaskCandidate> visible_tasks(std::vector<TaskCandidate> all, const std::uint32_t own_pid,
                                         const bool show_all) {
  std::erase_if(all, [&](const TaskCandidate &task) {
    return (task.pid && *task.pid == own_pid) ||
           (!show_all && (task.hidden || task.title.empty()));
  });
  return all;
}

std::string task_row_text(const TaskCandidate &task) {
  // Window text comes from any X client: tabs and line breaks must not add
  // browser columns or rows.
  const auto cell = [](std::string text) {
    std::ranges::replace_if(text, [](const char ch) { return ch == '\t' || ch == '\n' || ch == '\r'; }, ' ');
    return text;
  };
  const auto &program = !task.executable_name.empty() ? task.executable_name
                        : !task.class_name.empty()    ? task.class_name
                                                      : std::string("(unknown)");
  return cell(program) + "\t" + (task.title.empty() ? std::string("(untitled)") : cell(task.title)) +
         "\t" + (task.pid ? std::to_string(*task.pid) : std::string("-"));
}

std::string task_detail_text(const TaskCandidate &task) {
  if (!task.executable_path.empty()) return task.executable_path;
  if (!task.class_name.empty())
    return "Window class " + task.class_name + " (the executable is not readable)";
  return "This window publishes neither an executable nor a class.";
}

TaskApplied apply_task(layerc::core::GameProfile &profile, const TaskCandidate &task,
                       const bool exact_path, const bool unsaved) {
  auto &game = profile.game;
  if (task.executable_name.empty() && task.class_name.empty())
    return {StatusKind::failure, false,
            "This window publishes neither an executable nor a class; it cannot be matched."};
  game.global = false;
  game.window_title.clear();
  std::string status;
  if (!task.executable_name.empty()) {
    game.executable_name = task.executable_name;
    game.executable_path = exact_path ? task.executable_path : std::string{};
    game.window_class.clear();
    status = exact_path && !task.executable_path.empty()
                 ? "Application selected — matching its exact path."
                 : "Application selected — matching by executable name.";
  } else {
    game.executable_name.clear();
    game.executable_path.clear();
    game.window_class = task.class_name;
    status = "Application selected — matching by window class; its executable is not readable.";
  }
  if (unsaved && (profile.name == "New Profile" || profile.name.empty())) {
    const auto path = platform::path_from_utf8(task.executable_name.empty()
                                                  ? task.class_name : task.executable_name);
    const auto stem = path ? platform::path_to_utf8(path->stem()) : std::nullopt;
    if (stem && !stem->empty()) {
      profile.name = *stem;
      profile.id = layerc::core::slugify(profile.name, "profile");
    }
  }
  if (unsaved) status = "Application selected — Save once to create the profile.";
  return {task.executable_name.empty() ? StatusKind::warning : StatusKind::success,
          true, std::move(status)};
}

PlacementFrame placement_frame(const int x, const int y, const int width, const int height,
                               const int target_width, const int target_height) {
  if (target_width <= 0 || target_height <= 0 || width <= 0 || height <= 0)
    return {x, y, std::max(width, 0), std::max(height, 0), 1.0};
  const double scale = std::min(static_cast<double>(width) / target_width,
                                static_cast<double>(height) / target_height);
  const int fitted_width = std::max(1, static_cast<int>(target_width * scale));
  const int fitted_height = std::max(1, static_cast<int>(target_height * scale));
  return {x + (width - fitted_width) / 2, y + (height - fitted_height) / 2, fitted_width,
          fitted_height, scale};
}

PreviewPoint placed_origin(const PlacementFrame &frame, const double anchor_x,
                           const double anchor_y, const int panel_width, const int panel_height) {
  return {frame.x + static_cast<int>(std::lround(anchor_x * frame.width)) - panel_width / 2,
          frame.y + static_cast<int>(std::lround(anchor_y * frame.height)) - panel_height / 2};
}

Anchor anchor_at(const PlacementFrame &frame, const int x, const int y) {
  return {std::clamp(static_cast<double>(x - frame.x) / std::max(frame.width, 1), 0.0, 1.0),
          std::clamp(static_cast<double>(y - frame.y) / std::max(frame.height, 1), 0.0, 1.0)};
}

} // namespace layerc::editor
