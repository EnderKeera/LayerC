#include "diagnostic_text.hpp"
#include "tasks_window.hpp"
#include "daemon_link.hpp"
#include "x11_window.hpp"
#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>

#include <unistd.h>


namespace layerc::editor {
namespace {
constexpr int width = 640, height = 420, margin = 10;
constexpr int button_height = editor_button_height;
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
    if (!task.pid || *task.pid != static_cast<std::uint32_t>(getpid()))
      scan.windows.push_back(snapshot(task));
  const auto *found = platform::match_window(scan, match);
  if (found == nullptr) return std::nullopt;
  return candidate(*found);
}

bool task_matches(const TaskCandidate &task, const core::GameMatch &match) {
  return matching_task({task}, match).has_value();
}

TasksWindow::TasksWindow(Actions actions)
    : actions_(std::move(actions)),
      window_(std::make_unique<Fl_Double_Window>(width, height, "Detected Tasks — LayerE")) {
  window_->begin();
  list_ = new Fl_Hold_Browser(margin, margin + 20, width - 2 * margin, 220, "Detected tasks");
  list_->align(FL_ALIGN_TOP_LEFT);
  static const int columns[] = {150, 380, 0};
  list_->column_widths(columns);
  list_->column_char('\t');
  list_->format_char(0); // window titles are literal text, never @ formatting
  list_->callback(on_select, this);
  detail_ = new Fl_Box(margin, 255, width - 2 * margin, 40, "Executable");
  detail_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  detail_->box(FL_THIN_DOWN_BOX);
  show_all_ = new Fl_Check_Button(margin, 300, 200, 26, "Show all windows");
  show_all_->callback(on_refresh, this);
  exact_path_ = new Fl_Check_Button(220, 300, 260, 26, "Match the exact path");
  message_ = new Fl_Box(margin, 330, width - 2 * margin, 36);
  message_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  const int y = height - margin - button_height;
  auto *refresh_button = new Fl_Button(margin, y, 100, button_height, "&Refresh");
  refresh_button->callback(on_refresh, this);
  auto *use = new Fl_Button(margin + 110, y, 170, button_height, "&Use selected task");
  use->callback(on_use, this);
  auto *position = new Fl_Button(margin + 290, y, 170, button_height, "&Position on window");
  position->callback(on_position, this);
  auto *close = new Fl_Button(width - margin - 100, y, 100, button_height, "Close");
  close->callback(on_close, this);
  window_->callback(on_close, this);
  window_->end();
}

TasksWindow::~TasksWindow() = default;

void TasksWindow::show(const std::string &reason) {
  refresh();
  if (!reason.empty()) set_message(reason);
  window_->show();
}

void TasksWindow::hide() { window_->hide(); }

bool TasksWindow::shown() const { return window_->shown() != 0; }

void TasksWindow::set_message(const std::string &text) {
  message_->copy_label(text.c_str());
  message_->redraw();
}

void TasksWindow::refresh() {
  const auto previous = selected() ? selected()->window : 0;
  std::string error;
  tasks_ = visible_tasks(scan_tasks(error), static_cast<std::uint32_t>(getpid()),
                         show_all_->value() != 0);
  list_->clear();
  int select = 0;
  for (std::size_t index = 0; index < tasks_.size(); ++index) {
    list_->add(task_row_text(tasks_[index]).c_str());
    if (tasks_[index].window == previous) select = static_cast<int>(index) + 1;
  }
  list_->value(select);
  std::string message;
  if (!error.empty())
    message = "Window discovery: " + error;
  else if (tasks_.empty())
    message = show_all_->value() ? "No windows detected."
                                 : "No eligible windows detected. Show all windows includes "
                                   "hidden and untitled windows.";
  else
    message = std::to_string(tasks_.size()) + " windows detected.";
  // On Wayland the scan sees XWayland clients only; say so, never imply all.
  if (const auto note = wayland_scope_note(query_daemon_status(), wayland_environment());
      !note.empty())
    message = join_sentences(std::move(message), note);
  set_message(message);
  update_detail();
}

const TaskCandidate *TasksWindow::selected() const {
  const int row = list_ ? list_->value() : 0;
  return row > 0 && static_cast<std::size_t>(row) <= tasks_.size()
             ? &tasks_[static_cast<std::size_t>(row) - 1U]
             : nullptr;
}

void TasksWindow::update_detail() {
  const auto *task = selected();
  detail_->copy_label(task ? task_detail_text(*task).c_str()
                           : "Select a task to see its executable path.");
  detail_->redraw();
}

void TasksWindow::on_refresh(Fl_Widget *, void *data) { static_cast<TasksWindow *>(data)->refresh(); }
void TasksWindow::on_select(Fl_Widget *, void *data) { static_cast<TasksWindow *>(data)->update_detail(); }

void TasksWindow::on_use(Fl_Widget *, void *data) {
  auto &self = *static_cast<TasksWindow *>(data);
  const auto *selected = self.selected();
  if (!selected) { self.set_message("Select a task first."); return; }
  const TaskCandidate task = *selected; // the action may refresh the list
  if (self.actions_.use) self.actions_.use(task, self.exact_path_->value() != 0);
}

void TasksWindow::on_position(Fl_Widget *, void *data) {
  auto &self = *static_cast<TasksWindow *>(data);
  const auto *selected = self.selected();
  if (!selected) { self.set_message("Select a task first."); return; }
  const TaskCandidate task = *selected; // the action may refresh the list
  if (self.actions_.position) self.actions_.position(task);
}

void TasksWindow::on_close(Fl_Widget *, void *data) { static_cast<TasksWindow *>(data)->hide(); }

} // namespace layerc::editor
