#include "diagnostic_text.hpp"
#include "tasks_window.hpp"
#include "daemon_link.hpp"
#include "platform/services.hpp"
#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/platform.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>

#include <bit>
#include <utility>

namespace layerc::editor {
namespace {
constexpr int height = 420, margin = 10;
constexpr int button_height = editor_button_height;
} // namespace

PickOutcome decide_pick(const std::optional<TaskCandidate> &task, const std::uint32_t own_pid,
                        const TaskRequest request, const bool global, const bool matches) noexcept {
  if (!task || task->pid == own_pid) return PickOutcome::ineligible;
  if (request == TaskRequest::none) return PickOutcome::use;
  if (!global && !matches) return PickOutcome::mismatch;
  return request == TaskRequest::calibrate ? PickOutcome::calibrate : PickOutcome::position;
}

class TasksWindow::Browser final : public Fl_Hold_Browser {
public:
  Browser(const int width, TasksWindow &owner)
      : Fl_Hold_Browser(margin, margin + 20, width, 220, "Detected tasks"), owner_(owner) {}

  int handle(const int event) override {
    if (event == FL_KEYDOWN &&
        (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
      owner_.activate_selected(owner_.request_);
      return 1;
    }
    return Fl_Hold_Browser::handle(event);
  }

  [[nodiscard]] bool double_clicked_row() {
    int x{}, y{}, w{}, h{};
    bbox(x, y, w, h);
    return Fl::event() == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE &&
           Fl::event_is_click() && Fl::event_clicks() &&
           Fl::event_inside(x, y, w, h) && find_item(Fl::event_y()) != nullptr;
  }

private:
  TasksWindow &owner_;
};

TasksWindow *TasksWindow::picker_{};

TasksWindow::TasksWindow(Actions actions) : actions_(std::move(actions)) {
  const bool picking = window_picking_supported();
  const int width = picking ? 830 : 710;
  window_ = std::make_unique<Fl_Double_Window>(width, height, "Detected Tasks — LayerE");
  window_->begin();
  list_ = new Browser(width - 2 * margin, *this);
  list_->align(FL_ALIGN_TOP_LEFT);
  static const int columns[] = {150, 380, 0};
  list_->column_widths(columns);
  list_->column_char('\t');
  list_->format_char(0); // window titles are literal text, never @ formatting
  list_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE_ALWAYS);
  list_->callback(on_select, this);
  detail_ = new Fl_Box(margin, 255, width - 2 * margin, 40, "Executable");
  detail_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  detail_->box(FL_THIN_DOWN_BOX);
  show_all_ = new Fl_Check_Button(margin, 300, 200, editor_input_height, "Show all windows");
  show_all_->callback(on_refresh, this);
  exact_path_ = new Fl_Check_Button(220, 300, 260, editor_input_height, "Match the exact path");
  message_ = new Fl_Box(margin, 330, width - 2 * margin, 36);
  message_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  const int y = height - margin - button_height;
  auto *refresh_button = new Fl_Button(margin, y, 100, button_height, "&Refresh");
  refresh_button->callback(on_refresh, this);
  auto *use = new Fl_Button(margin + 110, y, 170, button_height, "&Use selected task");
  use->callback(on_use, this);
  auto *position = new Fl_Button(margin + 290, y, 170, button_height, "&Position on window");
  position->callback(on_position, this);
  auto *calibrate = new Fl_Button(margin + 470, y, 110, button_height, "&Calibrate");
  calibrate->callback(on_calibrate, this);
  if (picking) {
    auto *pick = new Fl_Button(margin + 590, y, 110, button_height, "Pick &window");
    pick->callback(on_pick, this);
  }
  auto *close = new Fl_Button(width - margin - 100, y, 100, button_height, "Close");
  close->callback(on_close, this);
  window_->callback(on_close, this);
  window_->end();
}

TasksWindow::~TasksWindow() { end_pick(false); }

void TasksWindow::show(const std::string &reason, const TaskRequest request) {
  end_pick(false);
  request_ = request;
  refresh();
  if (!reason.empty()) set_message(reason);
  window_->show();
}

void TasksWindow::hide() {
  end_pick(false);
  request_ = TaskRequest::none;
  window_->hide();
}

bool TasksWindow::shown() const { return window_->shown() != 0; }

void TasksWindow::set_message(const std::string &text) {
  message_->copy_label(text.c_str());
  message_->redraw();
}

void TasksWindow::refresh() {
  const auto previous = selected() ? selected()->window : 0;
  std::string error;
  auto scanned = actions_.scan ? actions_.scan(error) : scan_tasks(error);
  tasks_ = visible_tasks(std::move(scanned), current_process_id(), show_all_->value() != 0);
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
  if (!actions_.scan) {
    if (const auto note = wayland_scope_note(query_daemon_status(), wayland_environment());
        !note.empty())
      message = join_sentences(std::move(message), note);
  }
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

void TasksWindow::on_select(Fl_Widget *, void *data) {
  auto &self = *static_cast<TasksWindow *>(data);
  self.update_detail();
  if (self.list_->double_clicked_row()) self.activate_selected(self.request_);
}

void TasksWindow::on_use(Fl_Widget *, void *data) {
  static_cast<TasksWindow *>(data)->activate_selected(TaskRequest::none);
}

void TasksWindow::on_position(Fl_Widget *, void *data) {
  static_cast<TasksWindow *>(data)->activate_selected(TaskRequest::position);
}

void TasksWindow::on_calibrate(Fl_Widget *, void *data) {
  static_cast<TasksWindow *>(data)->activate_selected(TaskRequest::calibrate);
}

void TasksWindow::on_pick(Fl_Widget *, void *data) { static_cast<TasksWindow *>(data)->begin_pick(); }
void TasksWindow::on_close(Fl_Widget *, void *data) { static_cast<TasksWindow *>(data)->hide(); }

void TasksWindow::activate_selected(const TaskRequest action) {
  const auto *selection = selected();
  if (!selection) { set_message("Select a task first."); return; }
  const TaskCandidate task = *selection; // the action may refresh the list
  run_action(action, task);
}

void TasksWindow::run_action(const TaskRequest action, const TaskCandidate &task) {
  if (request_ == action) request_ = TaskRequest::none;
  switch (action) {
  case TaskRequest::none:
    if (actions_.use) actions_.use(task, exact_path_->value() != 0);
    break;
  case TaskRequest::position:
    if (actions_.position) actions_.position(task);
    break;
  case TaskRequest::calibrate:
    if (actions_.calibrate) actions_.calibrate(task);
    break;
  }
}

void TasksWindow::begin_pick() {
  if (picker_ || !actions_.pick_owner || !actions_.pick_owner->shown()) return;
  pick_was_shown_ = shown();
  pick_request_ = std::exchange(request_, TaskRequest::none);
  previous_dispatch_ = Fl::event_dispatch();
  previous_grab_ = Fl::grab();
  if (previous_grab_) Fl::grab(nullptr);
  window_->hide();
  Fl::first_window(actions_.pick_owner);
  picker_ = this;
  Fl::event_dispatch(dispatch_pick);
  Fl::grab(actions_.pick_owner);
  capture_watch_ = watch_pick_capture(std::bit_cast<std::uintptr_t>(fl_xid(actions_.pick_owner)),
                                      {Fl::add_timeout, Fl::remove_timeout}, [this] { cancel_pick(); });
  if (actions_.report)
    actions_.report(StatusKind::information,
                    "Pick window active — click a game window, or press Esc to cancel.");
}

void TasksWindow::end_pick(const bool reshow) {
  if (picker_ != this) return;
  picker_ = nullptr;
  Fl::event_dispatch(previous_dispatch_);
  capture_watch_.reset();
  Fl::grab(nullptr);
  if (previous_grab_ && previous_grab_->shown()) {
    Fl::first_window(previous_grab_);
    Fl::grab(previous_grab_);
  }
  previous_dispatch_ = nullptr;
  previous_grab_ = nullptr;
  if (reshow && pick_was_shown_) {
    request_ = pick_request_;
    window_->show();
  }
  pick_was_shown_ = false;
  pick_request_ = TaskRequest::none;
}

void TasksWindow::finish_pick() {
  const auto request = pick_request_;
  end_pick(false);
  const auto task = task_under_cursor();
  const auto game = actions_.game_match ? actions_.game_match() : std::nullopt;
  const auto outcome = decide_pick(task, current_process_id(), request,
                                   game && game->global, game && task && task_matches(*task, *game));
  switch (outcome) {
  case PickOutcome::ineligible:
    if (actions_.report) actions_.report(StatusKind::failure, "No eligible window was picked.");
    break;
  case PickOutcome::mismatch:
    if (actions_.report) actions_.report(StatusKind::failure, "The picked window does not match this profile.");
    break;
  case PickOutcome::use: run_action(TaskRequest::none, *task); break;
  case PickOutcome::position: run_action(TaskRequest::position, *task); break;
  case PickOutcome::calibrate: run_action(TaskRequest::calibrate, *task); break;
  }
}

void TasksWindow::cancel_pick() {
  end_pick(true);
  if (actions_.report) actions_.report(StatusKind::information, "Window picking cancelled.");
}

int TasksWindow::dispatch_pick(const int event, Fl_Window *window) {
  auto &picker = *picker_;
  const auto dispatch = picker.previous_dispatch_;
  switch (event) {
  case FL_PUSH:
    return 1;
  case FL_RELEASE:
    if (Fl::event_button() == FL_LEFT_MOUSE) picker.finish_pick();
    return 1;
  case FL_KEYDOWN:
  case FL_SHORTCUT:
    if (Fl::event_key() == FL_Escape) picker.cancel_pick();
    return 1;
  case FL_CLOSE:
  case FL_HIDE:
    if (window == picker.actions_.pick_owner) picker.end_pick(false);
    break;
  case FL_DRAG:
  case FL_MOVE:
  case FL_MOUSEWHEEL:
    return 1;
  default:
    break;
  }
  return dispatch ? dispatch(event, window) : Fl::handle_(event, window);
}

} // namespace layerc::editor
