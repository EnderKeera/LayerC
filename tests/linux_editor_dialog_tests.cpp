// Modal dialog and appearance cases of the Linux editor widget suite, split from
// linux_editor_widget_tests.cpp to keep translation units focused.
#include "cooldown_groups_window.hpp"
#include "appearance_window.hpp"
#include "accessible_widgets.hpp"
#include "automation_simulator.hpp"
#include "editor_app.hpp"
#include "editor_appearance.hpp"
#include "editor_menus.hpp"
#include "export_selection_window.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "linux_editor_test_access.hpp"
#include "profile_edit.hpp"
#include "editor_test_environment.hpp"
#include "platform/services.hpp"
#include "steady_wake.hpp"
#include "tasks_window.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>
#ifdef _WIN32
#include <FL/platform.H>

#include <array>
#include <iostream>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::editor {

struct EditorFolderTestAccess {
  static void set_start(EditorApp &app, std::function<bool(const std::filesystem::path &)> start) {
    app.start_folder_for_test_ = std::move(start);
  }
};

} // namespace layerc::editor

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using layerc::test::expect;

class Fixture final {
public:
  Fixture()
      : temp_("layerc-editor-widget", layerc::test::EditorTestScope::widget,
              "fixture control namespace is private") {
    path = platform::profile_directory() / "fixture.json";
  }
  void store() {
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(platform::write_profile_file(path, core::serialize_profile_json(profile)).ok(),
           "store only a private fixture");
  }
  std::filesystem::path path;
private:
  layerc::test::EditorTestEnvironment temp_;
};

template <class Widget = Fl_Button>
Widget *find_control(Fl_Group &group, const std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *control = dynamic_cast<Widget *>(child);
        control && control->visible_r() && control->label() && label == control->label())
      return control;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *control = find_control<Widget>(*nested, label)) return control;
  }
  return nullptr;
}

Fl_Button *find_button(Fl_Group &group, const std::string &label) {
  return find_control(group, label);
}

using namespace std::chrono_literals;
using Timer = editor::SteadyTimeout;

class FakeTimerQueue final {
public:
  struct Call {
    double delay{};
    Fl_Timeout_Handler handler{};
    void *data{};

    void invoke() const {
      if (expect(handler != nullptr, "the fake scheduler has a callback to deliver"))
        handler(data);
    }
  };

  FakeTimerQueue() { active_ = this; }
  ~FakeTimerQueue() { active_ = nullptr; }

  static Timer::Clock::time_point now() { return active_->now_; }
  static Timer::Scheduler scheduler() { return {add, remove}; }
  void advance(Timer::Clock::duration elapsed) { now_ += elapsed; }
  [[nodiscard]] bool pending() const { return call_.handler != nullptr; }
  [[nodiscard]] bool waiting_for(std::chrono::milliseconds delay) const {
    return pending() &&
        std::abs(call_.delay - std::chrono::duration<double>(delay).count()) < 1e-8;
  }
  [[nodiscard]] Call take() { return std::exchange(call_, {}); }
  [[nodiscard]] Call queued() const { return call_; }

private:
  static inline FakeTimerQueue *active_{};
  Timer::Clock::time_point now_{};
  Call call_{};

  static void add(double delay, Fl_Timeout_Handler handler, void *data) {
    expect(!active_->pending(), "a one-shot must dequeue or cancel before arming again");
    active_->call_ = {delay, handler, data};
  }
  static void remove(Fl_Timeout_Handler handler, void *data) {
    expect(active_->call_.handler == handler && active_->call_.data == data,
           "cancel must remove the matching pending callback");
    active_->call_ = {};
  }
};

} // namespace

void linux_editor_steady_timeout() {
  FakeTimerQueue queue;
  int fired{};
  {
    Timer timer([&] { ++fired; }, FakeTimerQueue::now, FakeTimerQueue::scheduler());
    timer.start(400ms);
    expect(timer.pending() && queue.waiting_for(400ms), "start arms the requested delay");
    queue.advance(150ms);
    queue.take().invoke();
    expect(fired == 0 && timer.pending() && queue.waiting_for(250ms),
           "an early wake re-arms just the steady remainder");
    queue.advance(-200ms);
    queue.take().invoke();
    expect(fired == 0 && timer.pending() && queue.waiting_for(450ms),
           "a backward clock reading cannot fire the callback before the deadline");
    queue.advance(450ms);
    const auto due = queue.take();
    due.invoke();
    due.invoke();
    expect(fired == 1 && !timer.pending() && !queue.pending(), "the due callback fires exactly once");

    timer.start(10ms);
    const auto cancelled = queue.queued();
    timer.cancel();
    cancelled.invoke();
    expect(fired == 1 && !timer.pending() && !queue.pending(), "cancel leaves no pending callback");

    timer.start(10ms);
    timer.start(30ms);
    expect(timer.pending() && queue.waiting_for(30ms), "restart replaces the pending callback");
    queue.advance(10ms);
    queue.take().invoke();
    expect(fired == 1 && queue.waiting_for(20ms), "the replaced deadline is no longer due");
    queue.advance(20ms);
    queue.take().invoke();
    expect(fired == 2 && !timer.pending() && !queue.pending(), "only the replacement fires");
    timer.start(10ms);
  }
  expect(fired == 2 && !queue.pending(), "destruction cancels a pending callback");
}

void steady_wake_delivers_on_monotonic_deadline() {
  struct State {
    int calls{};
    Timer::Clock::time_point fired_at{};
  } state;
  const Fl_Timeout_Handler handler = [](void *data) {
    auto &s = *static_cast<State *>(data);
    if (++s.calls == 1) s.fired_at = Timer::Clock::now();
  };
  const auto wait_until = [](auto deadline, auto done) {
    while (!done() && Timer::Clock::now() < deadline) Fl::wait(0.05);
  };
  const auto started = Timer::Clock::now();
  editor::steady_wake_add(0.08, handler, &state);
  expect(editor::steady_wake_armed(handler, &state), "positive delay armed an OS wake");
  expect(!Fl::has_timeout(handler, &state), "no wall-clock FLTK timeout while waiting");
  wait_until(started + 3s, [&] { return state.calls != 0; });
  expect(state.calls == 1 && state.fired_at - started >= 64ms &&
             state.fired_at - started <= 1080ms,
         "the 80 ms wake fired once within one tick early and one second late");
  expect(!editor::steady_wake_armed(handler, &state) && !Fl::has_timeout(handler, &state),
         "delivery leaves neither an OS wake nor a posted deferral");

  editor::steady_wake_add(0.08, handler, &state);
  editor::steady_wake_remove(handler, &state);
  wait_until(Timer::Clock::now() + 250ms, [] { return false; });
  expect(state.calls == 1 && !editor::steady_wake_armed(handler, &state) &&
             !Fl::has_timeout(handler, &state), "remove cancels a waiting OS wake");

  editor::steady_wake_add(0.15, handler, &state);
  editor::steady_wake_add(0.05, handler, &state);
  expect(editor::steady_wake_armed(handler, &state) && !Fl::has_timeout(handler, &state),
         "replacement waits outside the wall-clock queue");
  wait_until(Timer::Clock::now() + 250ms, [] { return false; });
  expect(state.calls == 2 && !editor::steady_wake_armed(handler, &state) &&
             !Fl::has_timeout(handler, &state), "only the replacement wakes the handler");

  editor::steady_wake_add(0.0, handler, &state);
  expect(!editor::steady_wake_armed(handler, &state) && Fl::has_timeout(handler, &state) &&
             state.calls == 2, "zero delay posts to FLTK without an OS wait");
  Fl::wait(0.05);
  expect(state.calls == 3 && !Fl::has_timeout(handler, &state), "zero delay runs on the next FLTK wait");
  editor::steady_wake_add(0.0, handler, &state);
  editor::steady_wake_remove(handler, &state);
  Fl::wait(0.05);
  expect(state.calls == 3 && !Fl::has_timeout(handler, &state), "remove cancels a posted deferral");
}

void steady_timeout_default_uses_monotonic_wake() {
  int fired{};
  Timer::Clock::time_point fired_at{};
  std::optional<Timer> timer(std::in_place, [&] {
    ++fired;
    fired_at = Timer::Clock::now();
  });
  const auto handler = editor::EditorAppTestAccess::steady_timeout_handler();
  void *const data = &*timer;
  const auto idle = [&] {
    return !editor::steady_wake_armed(handler, data) && !Fl::has_timeout(handler, data);
  };
  const auto start = Timer::Clock::now();
  timer->start(60ms);
  expect(timer->pending() && editor::steady_wake_armed(handler, data) &&
             !Fl::has_timeout(handler, data), "the default scheduler arms a monotonic OS wake");
  while (fired == 0 && Timer::Clock::now() < start + 3s) Fl::wait(0.05);
  expect(fired == 1 && fired_at >= start + 60ms && !timer->pending() && idle(),
         "the default timeout fires only once at or after the steady deadline");

  timer->start(5s);
  expect(editor::steady_wake_armed(handler, data), "default timeout armed before cancel");
  timer->cancel();
  expect(!timer->pending() && idle(), "cancel leaves no wake or FLTK deferral");

  timer->start(5s);
  expect(editor::steady_wake_armed(handler, data), "default timeout armed before destruction");
  timer.reset();
  expect(idle(), "destruction leaves no wake or FLTK deferral");
}

void linux_editor_autosave_steady_deadline() {
  Fixture fixture;
  fixture.store();
  FakeTimerQueue queue;
  using Access = editor::EditorAppTestAccess;
  auto app = Access::with_timer_scheduler(FakeTimerQueue::now, FakeTimerQueue::scheduler());
  app.build();
  app.main_window()->show();
  Fl::check();
  const auto original = layerc::test::read_file(fixture.path);
  if (!expect(original && app.session.has_document(), "open the private autosave fixture")) return;
  const auto edit_name = [&](const char *name) {
    auto *field = Access::name(app);
    field->value(name);
    app.field_changed(field);
  };
  edit_name("Monotonic autosave edit");
  expect(Access::autosave_pending(app) && queue.waiting_for(400ms), "text edits arm the debounce");
  queue.advance(100ms);
  queue.take().invoke();
  expect(queue.waiting_for(300ms) && layerc::test::read_file(fixture.path) == original &&
             app.session.has_unsaved_changes(), "an early toolkit wake does not save");
  queue.advance(300ms);
  const auto due = queue.take();
  due.invoke();
  const auto saved = layerc::test::read_file(fixture.path);
  expect(saved && saved->find("Monotonic autosave edit") != std::string::npos &&
             !app.session.has_unsaved_changes() && !Access::autosave_pending(app) && !queue.pending(),
         "at 400 ms the debounced edit saves and leaves no second callback");
  due.invoke();
  expect(layerc::test::read_file(fixture.path) == saved && !queue.pending(),
         "duplicate delivery does not change the committed profile or re-arm");

  edit_name("Modal autosave edit");
  Fl_Window modal(160, 80, "Autosave fixture dialog");
  modal.end();
  modal.set_modal();
  modal.show();
  expect(Fl::modal() == &modal, "the autosave fixture has a shown modal window");
  queue.advance(400ms);
  queue.take().invoke();
  expect(Access::autosave_pending(app) && queue.waiting_for(100ms) &&
             layerc::test::read_file(fixture.path) == saved && app.session.has_unsaved_changes(),
         "a due autosave defers while the modal is shown");
  modal.hide();

  app.main_window()->hide();
  queue.advance(100ms);
  queue.take().invoke();
  expect(!Access::autosave_pending(app) && !queue.pending() &&
             layerc::test::read_file(fixture.path) == saved, "a hidden main window cancels without saving");

  app.main_window()->show();
  edit_name("Explicit close autosave edit");
  const auto before_close = queue.queued();
  app.main_window()->do_callback();
  const auto closed = layerc::test::read_file(fixture.path);
  expect(!app.shown() && !Access::autosave_pending(app) && !queue.pending() && closed &&
             closed->find("Explicit close autosave edit") != std::string::npos,
         "normal close still flushes explicitly and cancels the pending timer");
  queue.advance(400ms);
  before_close.invoke();
  expect(layerc::test::read_file(fixture.path) == closed && !queue.pending(),
         "no scheduled autosave runs after close");
}

void linux_editor_playback_steady_deadline() {
  Fixture fixture;
  fixture.store();
  FakeTimerQueue queue;
  using Access = editor::EditorAppTestAccess;
  auto app = Access::with_timer_scheduler(FakeTimerQueue::now, FakeTimerQueue::scheduler());
  app.build();
  app.main_window()->show();
  Fl::check();
  app.command(editor::EditorCommand::automation_page);
  auto &page = *Access::automation(app)->group();
  auto *view = find_control<Fl_Choice>(page, "Automation view");
  if (!expect(view != nullptr, "the playback fixture can select the simulator")) return;
  view->value(1);
  view->do_callback();
  auto *state = find_control<Fl_Multiline_Output>(page, "Simulator state");
  auto *play = find_button(page, "Play");
  if (!expect(play && state && std::string_view(state->value()).starts_with("Events: 0/1"),
              "the playback fixture starts with an unplayed root")) return;

  const auto interval = editor::simulator_interval(1);
  play->do_callback();
  expect(queue.waiting_for(interval), "Play arms its simulator interval");
  queue.advance(interval / 2);
  queue.take().invoke();
  expect(std::string_view(state->value()).starts_with("Events: 0/1") &&
             queue.waiting_for(interval - interval / 2), "an early wake does not advance playback");
  queue.advance(interval - interval / 2);
  queue.take().invoke();
  expect(std::string_view(state->value()).starts_with("Events: 1/1") &&
             std::string_view(play->label()) == "Play" && !queue.pending(),
         "the due wake advances exactly one root and playback goes idle");
  app.main_window()->hide();
}

void linux_editor_cooldown_group_dialog_transactions() {
  Fixture fixture; fixture.store();
  editor::EditorSession session(fixture.path.parent_path());
  expect(!session.open(0), "open the private group fixture");
  const auto original = session.document().profile();
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Add group")->do_callback();
    dialog.mode->value(1); dialog.mode->do_callback();
    dialog.duration->value("nan");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.shown() && !dialog.applied && session.document().profile() == original,
           "invalid group timing leaves the dialog and document intact");
    dialog.duration->value("2.5"); dialog.charges->value("3");
    dialog.recovery->value(1); dialog.retrigger->value(1);
    dialog.name->value("Shared charges");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied, "valid complete group edit applies");
  }
  const auto grouped = session.document().profile();
  expect(grouped.cooldown_groups.size() == 1 &&
             grouped.cooldown_groups.front().duration == core::Milliseconds(2500) &&
             grouped.cooldown_groups.front().max_charges == 3 &&
             grouped.cooldown_groups.front().recovery == core::RecoveryMode::sequential &&
             grouped.cooldown_groups.front().retrigger == core::RetriggerPolicy::restart,
         "the group dialog edits shared timing, charges, recovery and retrigger");
  expect(session.undo() && session.document().profile() == original &&
             session.redo() && session.document().profile() == grouped,
         "the entire group dialog is one undoable session edit");
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Add group")->do_callback();
    dialog.mode->value(1); dialog.mode->do_callback();
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.shown() && !dialog.applied && session.document().profile() == grouped,
           "overlapping shared pools are refused without changing the document");
    find_button(dialog, "Cancel")->do_callback();
  }
  expect(!session.edit([](auto &profile) {
    profile.cooldown_groups.front().extra_fields.emplace("future", core::json::Value(true));
  }), "add opaque group metadata to the fixture");
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    dialog.name->value("Renamed group");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied &&
               session.document().profile().cooldown_groups.front().extra_fields.contains("future"),
           "group field edits retain opaque metadata");
  }
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Remove group")->do_callback();
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied && session.document().profile().cooldown_groups.empty(),
           "removing a group is an explicit validated edit");
  }
}

void linux_editor_export_profile_checklist() {
  Fixture fixture;
  std::vector<platform::StoredProfile> entries(3);
  for (std::size_t i{}; i < entries.size(); ++i) {
    entries[i].profile.id = "profile-" + std::to_string(i);
    entries[i].profile.name = "Same visible name";
  }
  {
    editor::ExportSelectionDialog dialog(entries, 1); dialog.show(); Fl::check();
    expect(dialog.profiles.size() == 3 && dialog.profiles[1]->value() &&
               !dialog.profiles[0]->value(),
           "the export checklist defaults to the selected saved profile");
    find_button(dialog, "Select none")->do_callback();
    find_button(dialog, "Export")->do_callback();
    expect(dialog.shown() && !dialog.selection, "an empty export selection is refused");
    dialog.profiles[0]->value(1); dialog.profiles[2]->value(1);
    find_button(dialog, "Export")->do_callback();
    expect(dialog.selection == std::vector<std::size_t>({0, 2}),
           "the export checklist accepts an arbitrary noncontiguous subset");
  }
  {
    editor::ExportSelectionDialog dialog(entries, 1); dialog.show(); Fl::check();
    find_button(dialog, "Select all")->do_callback();
    expect(std::ranges::all_of(dialog.profiles, [](auto *item) { return item->value(); }),
           "Select all includes every profile");
    find_button(dialog, "Cancel")->do_callback();
    expect(!dialog.selection, "Cancel never returns an export intent");
  }
}

void linux_editor_system_theme_restores_tab_class_colors() {
  // FLTK picks its backend once per process; the first case must pin X11.
  Fixture fixture;
  Fl_Window window(400, 300);
  Fl_Tabs tabs(0, 0, 400, 300);
  Fl_Group first(0, 25, 400, 275, "First");
  Fl_Hold_Browser list(10, 60, 100, 100); first.end();
  Fl_Group second(0, 25, 400, 275, "Second"); second.end();
  tabs.end(); window.end();
  editor::EditorAppearance themed; themed.theme = editor::EditorTheme::teal;
  editor::apply_editor_appearance(themed, window);
  editor::EditorAppearance system; system.theme = editor::EditorTheme::system;
  editor::apply_editor_appearance(system, window);
  // Earlier cases may have shown the (Graphite) default; System restores
  // FLTK's startup palette, which these now hold.
  const auto background = Fl::get_color(FL_BACKGROUND_COLOR);
  const auto foreground = Fl::get_color(FL_FOREGROUND_COLOR);
  const auto selection = Fl::get_color(FL_SELECTION_COLOR);
  Fl_Widget *tab_widgets[]{&tabs, &first, &second};
  for (auto *widget : tab_widgets) {
    expect(Fl::get_color(widget->selection_color()) == background,
           "System restores the default tab background, not the list selection colour");
    expect(Fl::get_color(widget->labelcolor()) == foreground,
           "System restores readable foreground text on every tab");
  }
  expect(Fl::get_color(list.selection_color()) == selection,
         "System preserves the native list highlight colour");
}

namespace {

Fl_Window *detected_tasks_window() {
  for (auto *window = Fl::first_window(); window; window = Fl::next_window(window))
    if (std::string_view(window->label() ? window->label() : "") == "Detected Tasks — LayerE")
      return window;
  return nullptr;
}

Fl_Hold_Browser *task_rows(Fl_Window &window) {
  for (int index = 0; index < window.children(); ++index)
    if (auto *rows = dynamic_cast<Fl_Hold_Browser *>(window.child(index))) return rows;
  return nullptr;
}

editor::TaskCandidate task_fixture() {
  editor::TaskCandidate task;
  task.window = 101;
  task.title = "Fixture game";
  task.executable_name = "fixture.exe";
  return task;
}

struct FltkEventState {
  int event{Fl::e_number}, key{Fl::e_keysym}, x{Fl::e_x}, y{Fl::e_y},
      clicks{Fl::e_clicks}, is_click{Fl::e_is_click}, length{Fl::e_length};
  char *text{Fl::e_text};
  ~FltkEventState() {
    Fl::e_number = event; Fl::e_keysym = key; Fl::e_x = x; Fl::e_y = y;
    Fl::e_clicks = clicks; Fl::e_is_click = is_click;
    Fl::e_text = text; Fl::e_length = length;
  }
};

void enter_task(Fl_Hold_Browser &rows) {
  const FltkEventState restore;
  Fl::e_keysym = FL_Enter;
  expect(rows.handle(FL_KEYDOWN) == 1, "the task list handles Enter");
}

void double_click_task(Fl_Hold_Browser &rows) {
  const FltkEventState restore;
  Fl::e_x = rows.x() + 6;
  Fl::e_y = rows.y() + 6;
  Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
  Fl::e_clicks = 1;
  Fl::e_is_click = 1;
  Fl::e_number = FL_PUSH;
  expect(rows.handle(FL_PUSH) == 1, "the task list receives the second click");
  Fl::e_number = FL_RELEASE;
  expect(rows.handle(FL_RELEASE) == 1, "the task list receives its release");
}

} // namespace

void open_profiles_folder_passes_private_store_to_launcher() {
  Fixture fixture;
  const std::string utf8 = "\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xA9";
  const auto directory = platform::path_from_utf8(utf8);
  if (!expect(directory.has_value(), "decode the UTF-8 folder name")) return;
  const auto config = fixture.path.parent_path() / *directory;
  std::filesystem::create_directories(config);
  const layerc::test::ScopedEnvironment store(
      editor::windows_editor_keys ? "LAYERC_PROFILE_DIR" : "XDG_CONFIG_HOME", config);
  if (!expect(store.applied(), "override the profile directory in the private fixture")) return;
  std::vector<std::filesystem::path> requests;
  editor::EditorApp app;
  app.build();
  editor::EditorFolderTestAccess::set_start(app, [&](const auto &path) {
    requests.push_back(path);
    return requests.size() == 1;
  });
  app.command(editor::EditorCommand::open_folder);
  const auto expected = app.session.directory();
  const auto *status = editor::EditorAppTestAccess::status(app);
  const auto path_text = platform::path_to_utf8(expected);
  if (!expect(status && path_text && path_text->find(utf8) != std::string::npos,
              "the status and UTF-8 profile path exist")) return;
  expect(expected == platform::profile_directory() && requests == std::vector{expected} &&
             std::string_view(status->value()) == "Opening the profiles folder: " + *path_text,
         "Open profiles folder starts the file manager at the private store");
  app.command(editor::EditorCommand::open_folder);
  expect(requests == std::vector{expected, expected} &&
             std::string_view(status->value()) == "Could not start " +
                 std::string(editor::folder_opener_name()) + "; the profiles folder is " + *path_text,
         "a refused file manager launch leaves the folder path in the error status");
}

void choose_running_opens_detected_tasks() {
  Fixture fixture;
  editor::EditorApp app;
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  auto &bar = editor::EditorAppTestAccess::menus(app)->bar();
  const auto *item = bar.find_item("&Tools/Detected &tasks...");
  if (!expect(item && item->active(), "the FLTK Detected tasks command is available")) return;
  item->do_callback(&bar);
  auto *tasks = editor::EditorAppTestAccess::tasks_window(app);
  expect(tasks && tasks->shown() && detected_tasks_window(),
         "invoking the real menu command shows the app-owned task chooser");
}

void detected_tasks_refreshes_and_closes() {
  Fixture fixture;
  auto task = task_fixture();
  int scans = 0;
  editor::TasksWindow::Actions actions;
  actions.scan = [&](std::string &) {
    if (++scans == 1) return std::vector{task};
    auto next = task;
    next.window += 1;
    next.title = "Refreshed fixture";
    return std::vector{next};
  };
  editor::TasksWindow tasks(std::move(actions));
  tasks.show();
  auto *window = detected_tasks_window();
  if (!expect(window != nullptr, "the injected chooser is shown")) return;
  auto *rows = task_rows(*window);
  auto *refresh = find_button(*window, "&Refresh");
  auto *close = find_button(*window, "Close");
  if (!expect(rows && refresh && close, "the chooser exposes rows, Refresh and Close")) return;
  if (!expect(scans == 1 && rows->size() == 1, "opening scans for the initial row")) return;
  expect(std::string_view(rows->text(1)).find(task.title) != std::string_view::npos,
         "the original task appears");
  rows->value(1);
  refresh->do_callback();
  expect(scans == 2 && rows->size() == 1 && rows->value() == 0 &&
             std::string_view(rows->text(1)).find("Refreshed fixture") != std::string_view::npos,
         "Refresh repopulates rows and drops a vanished selection");
  close->do_callback();
  expect(!tasks.shown() && !detected_tasks_window(), "the Close button hides Detected Tasks");
}

void tasks_double_click_runs_requested_action() {
  using enum editor::TaskRequest;
  Fixture fixture;
  const auto task = task_fixture();
  std::vector<editor::TaskRequest> calls;
  editor::TasksWindow::Actions actions;
  actions.scan = [&task](std::string &) { return std::vector{task}; };
  actions.use = [&](const auto &picked, bool) {
    expect(picked == task, "Use selected receives the selected task");
    calls.push_back(none);
  };
  actions.position = [&](const auto &picked) {
    expect(picked == task, "Position receives the selected task");
    calls.push_back(position);
  };
  actions.calibrate = [&](const auto &picked) {
    expect(picked == task, "Calibrate receives the selected task");
    calls.push_back(calibrate);
  };
  editor::TasksWindow tasks(std::move(actions));
  for (const auto mode : {none, position, calibrate}) {
    for (const auto activate : {double_click_task, enter_task}) {
      calls.clear();
      tasks.show({}, mode);
      Fl::check();
      auto *window = detected_tasks_window();
      auto *rows = window ? task_rows(*window) : nullptr;
      if (!expect(window && rows && rows->size() == 1, "the task chooser shows the injected row")) return;
      rows->value(1);
      activate(*rows);
      activate(*rows);
      expect(calls == std::vector{mode, none},
             "each gesture runs the requested action once, then goes back to Use selected");
      tasks.show({}, position);
      window->do_callback();
      expect(!tasks.shown(), "the close callback hides the task chooser");
      calls.clear();
      enter_task(*rows);
      expect(calls == std::vector{none}, "closing clears the pending request");
    }
  }
}

void tasks_calibrate_button_uses_selected_task() {
  Fixture fixture;
  const auto task = task_fixture();
  std::vector<editor::TaskCandidate> calls;
  editor::TasksWindow::Actions actions;
  actions.scan = [&task](std::string &) { return std::vector{task}; };
  actions.calibrate = [&](const auto &picked) { calls.push_back(picked); };
  editor::TasksWindow tasks(std::move(actions));
  tasks.show();
  auto *window = detected_tasks_window();
  auto *button = window ? find_button(*window, "&Calibrate") : nullptr;
  auto *rows = window ? task_rows(*window) : nullptr;
  if (!expect(window && button && rows, "the chooser has a Calibrate button and a task list")) return;
  expect((find_button(*window, "Pick &window") != nullptr) == editor::window_picking_supported(),
         "the Pick window button appears only where point picking is supported");
  button->do_callback();
  bool needs_selection = false;
  for (int index = 0; index < window->children(); ++index)
    if (const auto *label = window->child(index)->label(); label &&
        std::string_view(label) == "Select a task first.") needs_selection = true;
  expect(calls.empty() && needs_selection, "Calibrate without a row explains the missing selection");
  rows->value(1);
  button->do_callback();
  expect(calls == std::vector{task}, "Calibrate passes the selected task to the action");
}

void decide_pick_rules() {
  using enum editor::TaskRequest;
  using editor::PickOutcome;
  auto task = task_fixture();
  constexpr std::uint32_t own = 13;
  expect(editor::decide_pick(std::nullopt, own, none, true, true) == PickOutcome::ineligible,
         "a missing window is ineligible");
  task.pid = own;
  expect(editor::decide_pick(task, own, position, true, true) == PickOutcome::ineligible,
         "an editor window cannot be picked even for a global profile");
  task.pid = 42;
  expect(editor::decide_pick(task, own, none, false, false) == PickOutcome::use,
         "plain picking uses the task without requiring an existing match");
  expect(editor::decide_pick(task, own, position, false, false) == PickOutcome::mismatch &&
             editor::decide_pick(task, own, calibrate, false, false) == PickOutcome::mismatch,
         "a non-matching window is refused for either pending request");
  expect(editor::decide_pick(task, own, position, true, false) == PickOutcome::position &&
             editor::decide_pick(task, own, calibrate, true, false) == PickOutcome::calibrate,
         "a global profile bypasses only the picker's match guard");
  expect(editor::decide_pick(task, own, position, false, true) == PickOutcome::position &&
             editor::decide_pick(task, own, calibrate, false, true) == PickOutcome::calibrate,
         "a matching window dispatches either pending request");
}

// The Panels page's overlay contrast line follows the typed colours, and the
// four colour fields carry it as their screen-reader description.
void panel_contrast_line_follows_colors() {
  using Access = editor::EditorAppTestAccess;
  Fixture fixture; fixture.store();
  editor::EditorApp app; app.build();
  Access::window(app).show(); Fl::check();
  auto *tabs = Access::tabs(app);
  auto *panel = Access::panel_tab(app);
  if (!expect(tabs && panel, "the Panels tab exists")) return;
  tabs->value(panel); app.tab_changed();
  auto *line = Access::contrast_line(app);
  auto *panels = Access::panels(app);
  if (!expect(line && panels, "the Panels page owns a contrast line")) return;
  // fill_lists selects the first panel whenever none is chosen yet.
  expect(panels->value() == 1, "the Panels page opens on its first panel");
  Fl_Input *fields[]{Access::fill(app), Access::background(app), Access::border(app),
                     Access::text_color(app)};
  if (!expect(fields[0] && fields[1] && fields[2] && fields[3], "all four panel colour fields exist")) return;
  const auto type = [&](const char *fill, const char *background, const char *border,
                        const char *text) {
    const char *values[]{fill, background, border, text};
    for (std::size_t i = 0; i < 4; ++i) { fields[i]->value(values[i]); app.field_changed(fields[i]); }
  };
  const auto described = [&](const std::string &text) {
    return std::ranges::all_of(fields, [&](const Fl_Input *field) {
      return editor::accessibility::accessible_description(*field) == text;
    });
  };
  auto *show_text = Access::show_text(app);
  if (!expect(show_text != nullptr, "the Show bar text checkbox exists")) return;
  show_text->value(1); app.field_changed(show_text);
  type("#606060", "#5A5A5A", "#646464", "#707070");
  const std::string faint =
      "Low contrast. Text is 1.2:1 on Fill and 1.3:1 on Background; aim for 4.5:1. "
      "Border is 1.1:1 on Background; aim for 3:1.";
  expect(line->visible() && line->label() && faint == line->label() && described(faint),
         "faint colours show the exact line and describe all four colour fields");
  fields[0]->value("#12"); app.field_changed(fields[0]);
  expect(line->visible() && line->label() && faint == line->label(),
         "an unparsable colour keeps the last line");
  type("#141414", "#141414", "#FFFFFF", "#FFFFFF");
  expect(!line->visible() && described(""),
         "readable colours hide the line and clear the descriptions");
  expect(app.session.document().profile().panels.front().bar.fill_color == 0x141414,
         "the colours are accepted edits; the line never blocks them");
  Access::window(app).hide();
}

#ifdef _WIN32
void task_at_screen_point_maps_fixture_window() {
  Fl_Window fixture(80, 80, 320, 180, "LayerC point-pick fixture");
  fixture.end();
  fixture.show();
  Fl::check();
  const auto hwnd = fl_xid(&fixture);
  if (!expect(hwnd && SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
              "make the private fixture unobstructed")) return;
  RECT client{};
  if (!expect(GetClientRect(hwnd, &client) != FALSE, "read the fixture client size")) return;
  POINT centre{client.right / 2, client.bottom / 2};
  if (!expect(ClientToScreen(hwnd, &centre) != FALSE, "get the fixture's physical centre")) return;
  const auto picked = editor::task_at_screen_point(static_cast<int>(centre.x), static_cast<int>(centre.y));
  expect(picked && picked->window == reinterpret_cast<std::uintptr_t>(hwnd) &&
              picked->pid == GetCurrentProcessId() && picked->title == "LayerC point-pick fixture" &&
              picked->width == client.right && picked->height == client.bottom,
         "the physical point maps the HWND, PID, title and client size");
  expect(!editor::task_at_screen_point(GetSystemMetrics(SM_XVIRTUALSCREEN) - 1,
                                      GetSystemMetrics(SM_YVIRTUALSCREEN) - 1),
         "a point outside the virtual desktop has no attachable window");
}

void tasks_picker_restores_capture() {
  Fixture fixture;
  constexpr int width = 200, height = 120;
  RECT work{};
  const bool fits = SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE &&
                    work.right - work.left >= width && work.bottom - work.top >= height;
  struct PointerTarget {
    HWND window;
    ~PointerTarget() { if (window) DestroyWindow(window); }
  } target{fits ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                L"LayerC picker pointer target", WS_POPUP | WS_VISIBLE | SS_NOTIFY,
                                work.left, work.top, width, height, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr) : nullptr};
  if (fits && !expect(target.window != nullptr, "create the disposable picker pointer target")) return;

  const auto before_editor = Fl::event_dispatch();
  for (const bool close_owner : {false, true}) {
    {
      editor::EditorApp app;
      app.build();
      auto *owner = app.main_window();
      owner->show();

      std::optional<POINT> centre;
      RECT owner_rect{};
      if (target.window && GetWindowRect(fl_xid(owner), &owner_rect)) {
        for (const POINT corner : std::array<POINT, 4>{{
                 {work.left, work.top}, {work.right - width, work.top},
                 {work.left, work.bottom - height}, {work.right - width, work.bottom - height}}}) {
          const RECT bounds{corner.x, corner.y, corner.x + width, corner.y + height};
          RECT overlap{};
          if (IntersectRect(&overlap, &bounds, &owner_rect)) continue;
          if (!expect(SetWindowPos(target.window, HWND_TOPMOST, corner.x, corner.y, width, height,
                                  SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
                      "place the disposable target without activation")) return;
          centre = POINT{corner.x + width / 2, corner.y + height / 2};
          break;
        }
      }
      if (!centre) {
        if (target.window) ShowWindow(target.window, SW_HIDE);
        std::cerr << "[skip] picker pointer probe: no primary work-area corner outside the editor\n";
      }
      const auto expect_pointer_restored = [&](const std::string &path) {
        const HWND capture = GetCapture();
        if (!expect(capture == nullptr, path + " leaves no native mouse capture"))
          std::cerr << "  capture=" << capture << '\n';
        if (!centre) return;
        const HWND hit = WindowFromPoint(*centre);
        if (!expect(hit == target.window, path + " gives the pointer back to the target")) {
          std::cerr << "  point=(" << centre->x << ", " << centre->y << "), hit=" << hit
                    << ", target=" << target.window << '\n';
        }
      };

      app.command(editor::EditorCommand::tasks);
      auto *tasks = editor::EditorAppTestAccess::tasks_window(app);
      auto *window = detected_tasks_window();
      auto *pick = window ? find_button(*window, "Pick &window") : nullptr;
      if (!expect(tasks && pick, "the editor opens a chooser with point picking")) return;
      const auto prior = Fl::event_dispatch();
      pick->do_callback();
      expect(!tasks->shown() && Fl::grab() == owner && Fl::event_dispatch() != prior &&
                 GetCapture() == fl_xid(owner),
             "picking hides the list and captures only the editor window");
      expect(std::string_view(editor::EditorAppTestAccess::status(app)->value()) ==
                 "Pick window active — click a game window, or press Esc to cancel.",
             "the editor announces the exact pick instructions");
      {
        const FltkEventState event_restore;
        Fl::e_keysym = FL_Escape;
        expect(Fl::handle(FL_KEYDOWN, owner) == 1, "Escape is handled by the picker");
      }
      expect(tasks->shown(), "Escape re-shows Detected Tasks");
      expect(!Fl::grab(), "Escape clears the FLTK grab");
      expect(Fl::event_dispatch() == prior, "Escape restores the previous dispatcher");
      expect(GetCapture() != fl_xid(owner), "Escape releases the editor's native capture");
      expect_pointer_restored("Escape");

      pick->do_callback();
      {
        const FltkEventState event_restore;
        Fl::e_keysym = FL_Button + FL_RIGHT_MOUSE;
        expect(Fl::handle(FL_PUSH, owner) == 1 && Fl::grab() == owner,
               "a right press is swallowed without finishing the pick");
        expect(Fl::handle(FL_RELEASE, owner) == 1 && Fl::grab() == owner,
               "a right release is swallowed without finishing the pick");
        Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
        expect(Fl::handle(FL_PUSH, owner) == 1 && Fl::grab() == owner,
               "a left press alone leaves the picker armed");
        expect(Fl::handle(FL_RELEASE, owner) == 1 && !Fl::grab() &&
                   Fl::event_dispatch() == prior && !tasks->shown(),
               "a left release finishes the pick and restores the dispatcher and grab");
      }
      expect_pointer_restored("Left release");

      tasks->show();
      pick->do_callback();
      const auto hwnd = fl_xid(owner);
      SendMessageW(hwnd, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(hwnd));
      Fl::check();
      expect(Fl::grab() == owner, "capture notification naming the owner does not cancel");
      SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
      expect(Fl::grab() == owner && !tasks->shown(), "capture loss queues cancellation without running it in WndProc");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (!tasks->shown() && std::chrono::steady_clock::now() < deadline) Fl::wait(0.01);
      expect(tasks->shown() && !Fl::grab() && Fl::event_dispatch() == prior && GetCapture() != hwnd,
             "capture loss re-shows Detected Tasks and restores the dispatcher and grab");
      expect(std::string_view(editor::EditorAppTestAccess::status(app)->value()) == "Window picking cancelled.",
             "capture loss reports the same cancellation as Escape");
      expect_pointer_restored("Capture loss");

      pick->do_callback();
      if (close_owner) {
        Fl::handle(FL_CLOSE, owner);
        expect(!owner->shown(), "closing the main window hides it");
        expect(!Fl::grab(), "closing the main window clears the FLTK grab before teardown");
        expect(Fl::event_dispatch() == prior, "closing the main window restores the previous dispatcher");
        expect_pointer_restored("Closing the main window");
      } else {
        SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
        expect(Fl::grab() == owner, "the pick and queued cancellation are still armed at teardown");
      }
    }
    Fl::check();
    expect(!Fl::grab(), "editor destruction leaves no grab behind");
    expect(Fl::event_dispatch() == before_editor, "editor destruction leaves no dispatcher behind");
  }
}

// Pick window needs both a watch and native capture. Direct drag is told to
// end before the grab, never when the watch could not be installed.
void tasks_pick_needs_capture_watch() {
  Fixture fixture;
  Fl_Window owner(60, 60, 240, 160, "LayerC pick owner fixture");
  owner.end();
  owner.show();
  Fl::check();
  struct StubWatch final : editor::PickCaptureWatch {
    explicit StubWatch(const bool captured) : captured_(captured) {}
    bool owns_capture() const noexcept override { return captured_; }
    bool captured_;
  };
  bool watch_available = false, capture_available = false;
  int watches = 0, starts = 0;
  std::vector<std::pair<editor::StatusKind, std::string>> reports;
  editor::TasksWindow::Actions actions;
  actions.scan = [](std::string &) { return std::vector<editor::TaskCandidate>{}; };
  actions.pick_owner = &owner;
  actions.report = [&](const editor::StatusKind kind, const std::string &text) {
    reports.emplace_back(kind, text);
  };
  actions.pick_starting = [&] {
    expect(!Fl::grab(), "Direct drag ends before Pick window takes the pointer");
    ++starts;
  };
  actions.watch_capture = [&](const std::uintptr_t window, std::function<void()>)
      -> std::unique_ptr<editor::PickCaptureWatch> {
    ++watches;
    expect(window == reinterpret_cast<std::uintptr_t>(fl_xid(&owner)) && !Fl::grab(),
           "the owner's capture watch attaches before any grab");
    if (!watch_available) return nullptr;
    return std::make_unique<StubWatch>(capture_available);
  };
  editor::TasksWindow tasks(std::move(actions));
  tasks.show();
  auto *window = detected_tasks_window();
  auto *pick = window ? find_button(*window, "Pick &window") : nullptr;
  if (!expect(pick != nullptr, "the chooser offers Pick window")) return;
  const auto prior = Fl::event_dispatch();

  pick->do_callback();
  expect(watches == 1 && starts == 0 && !Fl::grab() && GetCapture() != fl_xid(&owner) &&
             Fl::event_dispatch() == prior && tasks.shown(),
         "without a capture watch Pick window takes nothing and leaves Direct drag alone");
  expect(reports.size() == 1 && reports.back().first == editor::StatusKind::failure &&
             reports.back().second ==
                 "Pick window is unavailable: the editor could not watch its pointer "
                 "capture. Choose the window from the list instead.",
         "the refusal names the reason and the list alternative");

  watch_available = true;
  pick->do_callback();
  expect(watches == 2 && starts == 1 && !Fl::grab() && GetCapture() != fl_xid(&owner) &&
             Fl::event_dispatch() == prior && tasks.shown(),
         "if capture fails Pick window restores the chooser, dispatcher and grab");
  expect(reports.size() == 2 && reports.back().first == editor::StatusKind::failure &&
             reports.back().second ==
                 "Pick window is unavailable: the editor could not take pointer "
                 "capture. Choose the window from the list instead.",
         "failed acquisition has its own reason and does not report active");

  capture_available = true;
  pick->do_callback();
  expect(watches == 3 && starts == 2 && Fl::grab() == &owner && !tasks.shown(),
         "with a watch and capture Pick window arms after ending Direct drag");
  {
    const FltkEventState event_restore;
    Fl::e_keysym = FL_Escape;
    expect(Fl::handle(FL_KEYDOWN, &owner) == 1 && !Fl::grab() &&
               Fl::event_dispatch() == prior && tasks.shown(),
           "Escape cancels the armed pick");
  }
  tasks.hide();
}

// layer57 L57-03: a capture loss still waiting for its deferred notice ends
// the pick on the next release without choosing a window or taking capture
// back from the editor-thread window that holds it. A key other than a
// modifier ends the pick, then reaches the editor.
void tasks_pick_loss_and_keys() {
  Fixture fixture;
  struct KeyProbe final : Fl_Box {
    KeyProbe() : Fl_Box(10, 10, 80, 30, "probe") {}
    int handle(const int event) override {
      if (event == FL_FOCUS || event == FL_UNFOCUS) return 1;
      if (event == FL_KEYBOARD) { ++keys; return 1; }
      return Fl_Box::handle(event);
    }
    int keys = 0;
  };
  Fl_Window owner(60, 60, 240, 160, "LayerC pick owner fixture");
  auto *const probe = new KeyProbe;
  owner.end();
  owner.show();
  Fl::check();
  int actions_run = 0;
  std::vector<std::string> reports;
  editor::TasksWindow::Actions actions;
  actions.scan = [](std::string &) { return std::vector<editor::TaskCandidate>{}; };
  actions.pick_owner = &owner;
  actions.use = [&](const editor::TaskCandidate &, bool) { ++actions_run; };
  actions.position = [&](const editor::TaskCandidate &) { ++actions_run; };
  actions.calibrate = [&](const editor::TaskCandidate &) { ++actions_run; };
  actions.report = [&](editor::StatusKind, const std::string &text) { reports.push_back(text); };
  editor::TasksWindow tasks(std::move(actions));
  tasks.show();
  auto *window = detected_tasks_window();
  auto *pick = window ? find_button(*window, "Pick &window") : nullptr;
  if (!expect(pick != nullptr, "the chooser offers Pick window")) return;
  const auto prior = Fl::event_dispatch();
  const HWND hwnd = fl_xid(&owner);
  struct Successor {
    HWND window;
    ~Successor() {
      if (window && GetCapture() == window) ReleaseCapture();
      if (window) DestroyWindow(window);
    }
  } successor{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                              L"LayerC capture successor", WS_POPUP, 0, 0, 10, 10, nullptr,
                              nullptr, GetModuleHandleW(nullptr), nullptr)};
  if (!expect(successor.window != nullptr, "create the editor-thread capture successor")) return;

  pick->do_callback();
  if (!expect(Fl::grab() == &owner && GetCapture() == hwnd && !tasks.shown(),
              "the armed pick holds the owner's capture")) return;
  SetCapture(successor.window);
  expect(GetCapture() == successor.window, "an editor-thread window takes capture");
  {
    const FltkEventState event_restore;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    expect(Fl::handle(FL_RELEASE, &owner) == 1, "the picker takes the release");
  }
  expect(actions_run == 0 && !reports.empty() && reports.back() == "Window picking cancelled.",
         "a release after capture loss cancels instead of choosing a window");
  expect(!Fl::grab() && Fl::event_dispatch() == prior && tasks.shown(),
         "the cancelled pick restores the dispatcher and the chooser");
  expect(GetCapture() == successor.window, "ending the pick leaves the successor's capture");
  const auto reported = reports.size();
  for (int pass = 0; pass < 5; ++pass) Fl::wait(0.01);
  expect(reports.size() == reported && GetCapture() == successor.window && !Fl::grab(),
         "the deferred loss notice never runs after the pick ended");
  ReleaseCapture();

  // The chooser stays hidden, so ending this pick moves no keyboard focus.
  tasks.hide();
  (void)Fl::handle(FL_FOCUS, &owner);
  pick->do_callback();
  if (!expect(Fl::grab() == &owner && GetCapture() == hwnd, "the pick arms again")) return;
  Fl::focus(probe);
  {
    const FltkEventState event_restore;
    // An unused key reaches FLTK's case-swapping shortcut retry, which
    // rewrites the event text in place: give each key writable text.
    char no_text[] = "", letter[] = "a";
    Fl::e_text = no_text;
    Fl::e_length = 0;
    Fl::e_keysym = FL_Shift_L;
    (void)Fl::handle(FL_KEYDOWN, &owner);
    expect(Fl::grab() == &owner && !tasks.shown(), "a modifier alone leaves the pick armed");
    const int keys = probe->keys;
    Fl::e_text = letter;
    Fl::e_length = 1;
    Fl::e_keysym = 'a';
    expect(Fl::handle(FL_KEYDOWN, &owner) == 1 && probe->keys == keys + 1,
           "another key reaches the editor's focused widget");
  }
  expect(!Fl::grab() && GetCapture() != hwnd && Fl::event_dispatch() == prior && !tasks.shown() &&
             actions_run == 0 && reports.back() == "Window picking cancelled.",
         "another key first ends the pick and gives back capture");
}
#endif
