#pragma once

#include "task_list.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Check_Button;
class Fl_Double_Window;
class Fl_Widget;
class Fl_Window;

namespace layerc::editor {

// A bounded scan of mapped top-level windows through a short-lived X11
// connection of its own; `error` names why nothing could be scanned.
[[nodiscard]] std::vector<TaskCandidate> scan_tasks(std::string &error);
// The one window LayerD would attach the profile to (active window first),
// under the shared matcher; nullopt when none matches.
[[nodiscard]] std::optional<TaskCandidate>
matching_task(const std::vector<TaskCandidate> &tasks, const layerc::core::GameMatch &match);
[[nodiscard]] bool task_matches(const TaskCandidate &task, const layerc::core::GameMatch &match);

class PickCaptureWatch;

enum class TaskRequest { none, position, calibrate };
enum class PickOutcome { ineligible, mismatch, use, position, calibrate };
[[nodiscard]] PickOutcome decide_pick(const std::optional<TaskCandidate> &task,
                                      std::uint32_t own_pid, TaskRequest request,
                                      bool global, bool matches) noexcept;

// Modeless Detected Tasks: scanned on open and on Refresh only (no timers or
// threads), and its X11 connection closes as soon as each scan ends.
class TasksWindow final {
public:
  struct Actions {
    std::function<void(const TaskCandidate &, bool exact_path)> use;
    std::function<void(const TaskCandidate &)> position;
    std::function<void(const TaskCandidate &)> calibrate;
    Fl_Window *pick_owner{};
    std::function<void(StatusKind, const std::string &)> report;
    std::function<std::optional<core::GameMatch>()> game_match;
    std::function<std::vector<TaskCandidate>(std::string &)> scan;
    // Runs once Pick window is about to take the pointer (ends Direct drag).
    std::function<void()> pick_starting;
    // Test seam for the owner's capture watch; empty uses the platform's.
    std::function<std::unique_ptr<PickCaptureWatch>(std::uintptr_t owner,
                                                    std::function<void()> lost)>
        watch_capture;
  };
  explicit TasksWindow(Actions actions);
  ~TasksWindow();
  TasksWindow(const TasksWindow &) = delete;
  TasksWindow &operator=(const TasksWindow &) = delete;
  // `reason` explains a request (e.g. several matching windows); empty
  // for a plain open.
  void show(const std::string &reason = {}, TaskRequest request = TaskRequest::none);
  void refresh();
  void hide();
  [[nodiscard]] bool shown() const;
  [[nodiscard]] const TaskCandidate *selected() const;
  void set_message(const std::string &text);

private:
  static void on_refresh(Fl_Widget *, void *);
  static void on_select(Fl_Widget *, void *);
  static void on_use(Fl_Widget *, void *);
  static void on_position(Fl_Widget *, void *);
  static void on_calibrate(Fl_Widget *, void *);
  static void on_pick(Fl_Widget *, void *);
  static void on_close(Fl_Widget *, void *);
  void activate_selected(TaskRequest action);
  void run_action(TaskRequest action, const TaskCandidate &task);
  void update_detail();
  void begin_pick();
  void finish_pick();
  void cancel_pick();
  void end_pick(bool reshow);
  static int dispatch_pick(int event, Fl_Window *window);
  static TasksWindow *picker_;
  class Browser;
  Actions actions_;
  std::unique_ptr<Fl_Double_Window> window_;
  Browser *list_{};
  TaskRequest request_{TaskRequest::none};
  TaskRequest pick_request_{TaskRequest::none};
  int (*previous_dispatch_)(int, Fl_Window *){};
  Fl_Window *previous_grab_{};
  std::unique_ptr<PickCaptureWatch> capture_watch_;
  bool pick_was_shown_{};
  Fl_Check_Button *show_all_{}, *exact_path_{};
  Fl_Box *detail_{}, *message_{};
  std::vector<TaskCandidate> tasks_;
};

} // namespace layerc::editor
