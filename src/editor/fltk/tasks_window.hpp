#pragma once

#include "task_list.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Check_Button;
class Fl_Double_Window;
class Fl_Hold_Browser;
class Fl_Widget;

namespace layerc::editor {

// A bounded scan of mapped top-level windows through a short-lived X11
// connection of its own; `error` names why nothing could be scanned.
[[nodiscard]] std::vector<TaskCandidate> scan_tasks(std::string &error);
// The one window LayerD would attach the profile to (active window first),
// under the shared matcher; nullopt when none matches.
[[nodiscard]] std::optional<TaskCandidate>
matching_task(const std::vector<TaskCandidate> &tasks, const layerc::core::GameMatch &match);
[[nodiscard]] bool task_matches(const TaskCandidate &task, const layerc::core::GameMatch &match);

// Modeless Detected Tasks: scanned on open and on Refresh only (no timers or
// threads), and its X11 connection closes as soon as each scan ends.
class TasksWindow final {
public:
  struct Actions {
    std::function<void(const TaskCandidate &, bool exact_path)> use;
    std::function<void(const TaskCandidate &)> position;
  };
  explicit TasksWindow(Actions actions);
  ~TasksWindow();
  TasksWindow(const TasksWindow &) = delete;
  TasksWindow &operator=(const TasksWindow &) = delete;
  // `reason` explains a request (e.g. several matching windows); empty
  // for a plain open.
  void show(const std::string &reason = {});
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
  static void on_close(Fl_Widget *, void *);
  void update_detail();
  Actions actions_;
  std::unique_ptr<Fl_Double_Window> window_;
  Fl_Hold_Browser *list_{};
  Fl_Check_Button *show_all_{}, *exact_path_{};
  Fl_Box *detail_{}, *message_{};
  std::vector<TaskCandidate> tasks_;
};

} // namespace layerc::editor
