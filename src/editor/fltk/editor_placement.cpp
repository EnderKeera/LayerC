// Detected Tasks and live placement against a game window. LayerD has no
// on-window edit channel on Linux: the preview takes the target window's
// shape and every drag is published through the live draft, which moves
// the click-through overlay. Direct Drag would need an input-accepting
// overlay, which X11 overlays never are.
#include "daemon_link.hpp"
#include "editor_app.hpp"
#include "platform/product_text.hpp"
#include "tasks_window.hpp"

#include <FL/Fl_Button.H>

namespace layerc::editor {

void EditorApp::show_tasks(const std::string &reason) {
  if (!tasks_window) {
    tasks_window = std::make_unique<TasksWindow>(TasksWindow::Actions{
        [this](const TaskCandidate &task, bool exact_path) { use_task(task, exact_path); },
        [this](const TaskCandidate &task) { position_on(task); }});
  }
  tasks_window->show(reason);
}

void EditorApp::use_task(const TaskCandidate &task, const bool exact_path) {
  if (!session.has_document()) { report(StatusKind::failure, "Open or create a profile first."); return; }
  if (!apply_fields()) { report(StatusKind::failure, "Use selected task needs valid fields first."); return; }
  TaskApplied applied;
  const auto error = session.edit([&](auto &profile) {
    applied = apply_task(profile, task, exact_path, session.is_draft());
  });
  changed(error);
  if (error) return;
  report(applied.kind, applied.status);
  if (tasks_window) tasks_window->set_message(applied.status);
}

bool EditorApp::placement_active() const {
  return placement_ && session.has_document() &&
         session.document().profile().id == placement_profile_;
}

void EditorApp::position_command() {
  if (placement_active()) { end_placement(StatusKind::information, "Positioning on the window ended."); return; }
  if (!session.has_document()) return;
  if (session.is_draft()) {
    report(StatusKind::failure, "Save this new profile once before positioning it on a window.");
    return;
  }
  if (!apply_fields()) return;
  if (const auto target = window_target("Position on window")) position_on(*target);
}

std::optional<TaskCandidate> EditorApp::window_target(const std::string_view action) {
  std::string error;
  const auto tasks = scan_tasks(error);
  if (!error.empty() && tasks.empty()) {
    report(StatusKind::failure, "Window discovery failed: " + error);
    return std::nullopt;
  }
  auto game = session.document().profile().game;
  std::size_t matches = 0;
  for (const auto &task : tasks)
    if (task_matches(task, game)) ++matches;
  const auto *selected = tasks_window && tasks_window->shown() ? tasks_window->selected() : nullptr;
  if (selected && task_matches(*selected, game)) return *selected;
  if (matches == 1) {
    if (const auto target = matching_task(tasks, game)) return target;
  }
  const std::string reason = matches == 0
      ? "No running window matches this profile. Start the game, or check the profile's "
        "Application, then press Refresh."
      : "Several windows match — select one, then " + std::string(action) + ".";
  report(StatusKind::information, matches == 0 ? "No automatic match — select an eligible window in Detected Tasks."
                      : "Multiple windows match — select one in Detected Tasks.");
  show_tasks(reason);
  return std::nullopt;
}

void EditorApp::position_on(const TaskCandidate &task) {
  if (!session.has_document() || session.is_draft()) {
    report(StatusKind::failure, "Save this new profile once before positioning it on a window.");
    return;
  }
  if (!task_matches(task, session.document().profile().game)) {
    const std::string text = "That window does not match this profile. Use selected task first "
                             "to change the profile's Application.";
    report(StatusKind::failure, text);
    if (tasks_window) tasks_window->set_message(text);
    return;
  }
  if (task.width <= 0 || task.height <= 0) {
    report(StatusKind::failure, "That window has no visible size to position against.");
    return;
  }
  placement_ = task;
  placement_profile_ = session.document().profile().id;
  if (tasks_window) tasks_window->hide();
  update_position_button();
  preview->redraw();
  std::string text =
      "Positioning on " + (task.title.empty() ? std::string("the window") : "\"" + task.title + "\"") +
      " (" + std::to_string(task.width) + "x" + std::to_string(task.height) +
      "): " + placement_drag_note();
  // On Wayland the geometry is the XWayland window's; the note names the
  // degraded path and LayerD's own window provider.
  if (const auto note = wayland_scope_note(query_daemon_status(), wayland_environment());
      !note.empty())
    text += " " + note;
  report(StatusKind::information, text);
}

void EditorApp::end_placement(const StatusKind kind, const std::string &status) {
  placement_.reset();
  placement_profile_.clear();
  update_position_button();
  preview->redraw();
  if (!status.empty()) report(kind, status);
}

void EditorApp::update_position_button() {
  if (panel_page_) panel_page_->show_position(placement_active());
}

} // namespace layerc::editor
