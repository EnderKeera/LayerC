// Steady-timer arm failures in the editor widget suites (layer57 L57-08/L57-09).
// A refused OS wake never falls back to FLTK's wall-clock queue: the timeout
// stays owed, is retried only when the loop next runs, and the editor reports
// the fault once in its status column.
#include "editor_app.hpp"
#include "editor_test_environment.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "linux_editor_test_access.hpp"
#include "platform/services.hpp"
#include "profile_edit.hpp"
#include "steady_timeout.hpp"
#include "steady_wake.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>

#ifdef _WIN32
#include <windows.h>
#else
#include "platform/linux/fltk_fd.hpp"

#include <variant>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using layerc::test::expect;
using namespace std::chrono_literals;
using Timer = editor::SteadyTimeout;
using Clock = Timer::Clock;
using Access = editor::EditorAppTestAccess;
using editor::SteadyWakeFault;

constexpr std::string_view timer_warning = "System timers unavailable — autosave and refreshes retry when LayerE next handles events.";

// A toolkit queue of one-shots that refuses positive delays the way a failed
// OS wake does. Zero delays are always accepted, as the real scheduler posts
// them to FLTK.
class RefusingQueue final {
public:
  struct Call {
    double delay{};
    Fl_Timeout_Handler handler{};
    void *data{};
  };

  RefusingQueue() { active_ = this; }
  ~RefusingQueue() { active_ = nullptr; }
  RefusingQueue(const RefusingQueue &) = delete;
  RefusingQueue &operator=(const RefusingQueue &) = delete;

  static Clock::time_point now() { return active_->now_; }
  static Timer::Scheduler scheduler() { return {add, remove}; }
  void advance(const Clock::duration elapsed) { now_ += elapsed; }
  void refuse(const bool refuse) { refuse_ = refuse; }
  [[nodiscard]] std::size_t size() const { return calls_.size(); }
  [[nodiscard]] std::size_t zero_calls() const {
    std::size_t count{};
    for (const auto &call : calls_) count += call.delay > 0.0 ? 0U : 1U;
    return count;
  }
  [[nodiscard]] bool queued(const void *data, const bool zero) const {
    for (const auto &call : calls_)
      if (call.data == data && (call.delay > 0.0) != zero) return true;
    return false;
  }
  [[nodiscard]] Call call_for(const void *data) const {
    for (const auto &call : calls_)
      if (call.data == data) return call;
    return {};
  }
  // One toolkit pass over the posted zero-delay deliveries.
  void deliver_zero() {
    std::vector<Call> due;
    for (auto it = calls_.begin(); it != calls_.end();) {
      if (it->delay > 0.0) { ++it; continue; }
      due.push_back(*it);
      it = calls_.erase(it);
    }
    for (const auto &call : due) call.handler(call.data);
  }

private:
  static inline RefusingQueue *active_{};
  Clock::time_point now_{};
  bool refuse_{};
  std::vector<Call> calls_;

  static void drop(Fl_Timeout_Handler handler, void *data) {
    std::erase_if(active_->calls_, [&](const Call &call) {
      return call.handler == handler && call.data == data;
    });
  }
  static bool add(const double delay, Fl_Timeout_Handler handler, void *data) {
    drop(handler, data);
    if (active_->refuse_ && delay > 0.0) return false;
    active_->calls_.push_back({delay, handler, data});
    return true;
  }
  static void remove(Fl_Timeout_Handler handler, void *data) { drop(handler, data); }
};

struct FaultLog {
  int calls{};
  bool last{};
  static void note(void *data) {
    auto &log = *static_cast<FaultLog *>(data);
    ++log.calls;
    log.last = editor::steady_timers_failing();
  }
};

bool retry_listed() { return Fl::has_check(Access::steady_retry_handler()) != 0; }
void retry_pass() { Access::steady_retry_handler()(nullptr); }

// Leaves later cases with working timers: injection off and one successful
// positive arm, the only thing that clears the shared fault.
void restore_healthy_timers() {
  editor::steady_wake_fault_for_test(SteadyWakeFault::none);
#ifndef _WIN32
  editor::fltk_fd_limit_for_test(std::nullopt);
#endif
  Timer probe([] {});
  probe.start(1h);
  probe.cancel();
  expect(!editor::steady_timers_failing() && !retry_listed(),
         "the case leaves timers healthy and no retry check behind");
}

// Counts loop passes over `span`: a posted zero timeout or idle callback would
// spin thousands of times; an idle-blocking loop returns only for real events.
int loop_passes(const Clock::duration span) {
  int passes{};
  const auto end = Clock::now() + span;
  for (auto now = Clock::now(); now < end; now = Clock::now()) {
    // FLTK rounds a Windows wait to whole milliseconds, so a tail under half a
    // millisecond would not block. A due FLTK timeout still returns at once.
    Fl::wait(std::max(std::chrono::duration<double>(end - now).count(), 0.001));
    ++passes;
  }
  return passes;
}

// The fewest loop passes in any of three 100 ms windows. A burst of unrelated
// desktop messages lands in one window; a spin, or a retry that re-arms a
// short timer (Windows fires one at most every 10 ms), shows in all three.
int idle_passes() {
  int fewest = loop_passes(100ms);
  for (int window = 1; window < 3; ++window) fewest = std::min(fewest, loop_passes(100ms));
  return fewest;
}

std::string passes_text(const std::string_view claim, const int passes) {
  return std::string(claim) + " (" + std::to_string(passes) + " loop passes)";
}

#ifndef _WIN32
std::size_t open_descriptors() {
  std::size_t count{};
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)entry;
    ++count;
  }
  return count;
}
#endif

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

std::string status_text(editor::EditorApp &app) {
  const auto *status = Access::status(app);
  return status && status->value() ? std::string(status->value()) : std::string{};
}

} // namespace

// A: each failing OS stage refuses a positive wake and queues nothing.
void steady_wake_refuses_without_wall_clock_fallback() {
  int calls{};
  const Fl_Timeout_Handler handler = [](void *data) { ++*static_cast<int *>(data); };
  const auto idle = [&] {
    return !editor::steady_wake_armed(handler, &calls) && !Fl::has_timeout(handler, &calls);
  };
  for (const auto fault : {SteadyWakeFault::create, SteadyWakeFault::arm}) {
#ifndef _WIN32
    const auto before = open_descriptors();
#endif
    editor::steady_wake_fault_for_test(fault);
    const bool added = editor::steady_wake_add(0.08, handler, &calls);
    expect(!added && idle(), "a refused positive wake queues no wall-clock FLTK timeout");
#ifndef _WIN32
    expect(open_descriptors() == before, "the refused timerfd is closed");
#endif
    expect(editor::steady_wake_add(0.0, handler, &calls) && Fl::has_timeout(handler, &calls) &&
               !editor::steady_wake_armed(handler, &calls),
           "a zero delay still posts its deferral during the fault");
    editor::steady_wake_remove(handler, &calls);
    expect(idle() && calls == 0, "remove withdraws the posted deferral");
  }
  editor::steady_wake_fault_for_test(SteadyWakeFault::none);
  expect(editor::steady_wake_add(0.08, handler, &calls) &&
             editor::steady_wake_armed(handler, &calls) && !Fl::has_timeout(handler, &calls),
         "with the fault gone a positive wake arms the OS timer");
  editor::steady_wake_remove(handler, &calls);
  expect(idle() && calls == 0, "remove leaves the handler idle");
  restore_healthy_timers();
}

// B: a descriptor FLTK's select loop cannot represent is closed or reported,
// never registered.
void linux_fd_range_refuses_unwatchable_descriptors() {
#ifndef _WIN32
  Fixture fixture; // the live-draft client below uses only private namespaces
  int calls{};
  const Fl_Timeout_Handler handler = [](void *data) { ++*static_cast<int *>(data); };
  expect(editor::fltk_fd_watchable(0) && !editor::fltk_fd_watchable(-1) &&
             !editor::fltk_fd_watchable(FD_SETSIZE),
         "the watchable range is what select can represent");
  const auto before = open_descriptors();
  editor::fltk_fd_limit_for_test(0);
  expect(!editor::fltk_fd_watchable(0), "the test limit rejects ordinary descriptors");
  expect(!editor::steady_wake_add(0.08, handler, &calls) &&
             !editor::steady_wake_armed(handler, &calls) && !Fl::has_timeout(handler, &calls) &&
             open_descriptors() == before,
         "an unwatchable timerfd is closed instead of registered or replaced by a wall-clock timeout");
  {
    auto drafts = editor::make_standalone_live_drafts([] {});
    const auto *unavailable = std::get_if<editor::StandaloneLiveDraftsUnavailable>(&drafts);
    expect(unavailable && unavailable->reason ==
               "the live draft notification is outside the range LayerE can watch",
           "live drafts report an unwatchable notification instead of watching it");
  }
  editor::fltk_fd_limit_for_test(std::nullopt);
  auto drafts = editor::make_standalone_live_drafts([] {});
  expect(std::holds_alternative<std::unique_ptr<editor::StandaloneLiveDrafts>>(drafts),
         "live drafts watch an ordinary notification descriptor");
  restore_healthy_timers();
#else
  std::cout << "Not applicable on Windows: the FLTK descriptor range is tested on Linux.\n";
#endif
}

// C: a refused timeout is owed, retried on loop passes and delivered through
// the toolkit, never from the retry check.
void steady_timeout_retries_refused_wakes() {
  if (!expect(!editor::steady_timers_failing() && !retry_listed(), "timers start healthy"))
    return;
  RefusingQueue queue;
  FaultLog log;
  editor::set_steady_timer_fault_handler(&FaultLog::note, &log);
  int fired{};
  {
    Timer timer([&] { ++fired; }, RefusingQueue::now, RefusingQueue::scheduler());
    queue.refuse(true);
    timer.start(1s);
    expect(!timer.pending() && timer.owed() && editor::steady_timers_failing() &&
               retry_listed() && queue.size() == 0,
           "a refused wake is owed and listed for retry with nothing queued");
    expect(log.calls == 0, "the fault is reported after the operation, not inside it");
    Fl::check();
    expect(log.calls == 1 && log.last, "one deferred report says timers are failing");
    timer.start(2s);
    Fl::check();
    expect(log.calls == 1 && timer.owed() && retry_listed(),
           "re-starting a retrying timeout reports nothing new");

    queue.advance(1500ms);
    retry_pass();
    expect(fired == 0 && timer.owed() && !timer.pending() && queue.size() == 0,
           "a pass before the steady deadline neither fires nor queues");
    queue.advance(600ms);
    retry_pass();
    expect(fired == 0 && timer.pending() && queue.queued(&timer, true) && !retry_listed(),
           "past the deadline a pass posts the zero delivery without calling the owner");
    queue.deliver_zero();
    expect(fired == 1 && !timer.owed() && queue.size() == 0,
           "toolkit delivery calls the owner once and leaves the timeout idle");
    expect(editor::steady_timers_failing(), "a zero-delay delivery does not clear the fault");

    int first_fired{}, second_fired{};
    Timer first([&] { ++first_fired; }, RefusingQueue::now, RefusingQueue::scheduler());
    auto second = std::make_unique<Timer>([&] { ++second_fired; }, RefusingQueue::now,
                                          RefusingQueue::scheduler());
    first.start(100ms);
    second->start(100ms);
    queue.advance(200ms);
    retry_pass();
    expect(queue.zero_calls() == 2 && first.pending() && second->pending() && !retry_listed(),
           "both due timeouts post their zero deliveries");
    const auto stale = queue.call_for(&first);
    first.cancel();
    second.reset();
    queue.deliver_zero();
    stale.handler(stale.data);
    expect(first_fired == 0 && second_fired == 0 && queue.size() == 0,
           "a timeout cancelled or destroyed after its delivery was posted never runs");

    timer.start(1s);
    expect(timer.owed() && !timer.pending() && retry_listed(), "the timeout is refused again");
    timer.cancel();
    expect(!timer.owed() && !retry_listed() && editor::steady_timers_failing(),
           "cancel withdraws the retry but does not clear the shared fault");
    Fl::check();
    expect(log.calls == 1, "cancel reports nothing");

    timer.start(1s);
    queue.refuse(false);
    retry_pass();
    expect(timer.pending() && queue.queued(&timer, false) && !editor::steady_timers_failing() &&
               !retry_listed(),
           "recovery schedules the owed timeout and clears the fault");
    expect(log.calls == 1, "recovery is reported after the pass, not inside it");
    Fl::check();
    expect(log.calls == 2 && !log.last, "recovery is reported once");
    Fl::check();
    expect(log.calls == 2 && fired == 1, "no further reports or deliveries");
  }
  expect(queue.size() == 0, "destruction cancels the scheduled timeout");
  editor::set_steady_timer_fault_handler(nullptr, &log);
  restore_healthy_timers();
}

// D: an owed or recovered timeout leaves an otherwise idle loop blocking.
void steady_timer_retry_keeps_loop_idle() {
  int fired{};
  Timer timer([&] { ++fired; });
  const auto handler = Access::steady_timeout_handler();
  editor::steady_wake_fault_for_test(SteadyWakeFault::create);
  timer.start(5s);
  expect(timer.owed() && !timer.pending() && retry_listed() &&
             !editor::steady_wake_armed(handler, &timer) && !Fl::has_timeout(handler, &timer),
         "a refused default timeout is owed without any FLTK timeout");
  const auto owed_passes = idle_passes();
  expect(owed_passes <= 3 && fired == 0,
         passes_text("the retry check runs only when the loop wakes, so an idle loop still blocks",
                     owed_passes));
  timer.cancel();
  expect(!timer.owed() && !retry_listed(), "cancel withdraws the retry check");
  const auto cancelled_passes = idle_passes();
  expect(cancelled_passes <= 3 && fired == 0,
         passes_text("after cancel the loop blocks", cancelled_passes));

  timer.start(5s);
  editor::steady_wake_fault_for_test(SteadyWakeFault::none);
  retry_pass();
  expect(timer.pending() && editor::steady_wake_armed(handler, &timer) && !retry_listed() &&
             !editor::steady_timers_failing(),
         "recovery arms the OS wake and withdraws the check");
  const auto recovered_passes = idle_passes();
  expect(recovered_passes <= 3 && fired == 0,
         passes_text("after recovery the loop blocks until the deadline", recovered_passes));
  timer.cancel();
  restore_healthy_timers();
}

// E: the editor shows and speaks a sustained fault once and still saves on its
// steady deadline.
void editor_reports_refused_timers_once() {
  Fixture fixture;
  fixture.store();
  RefusingQueue queue;
  {
    auto app = Access::with_timer_scheduler(RefusingQueue::now, RefusingQueue::scheduler());
    app.build();
    app.main_window()->show();
    Fl::check();
    const auto original = layerc::test::read_file(fixture.path);
    if (!expect(original && app.session.has_document(), "open the private autosave fixture"))
      return;
    const auto warned = [&] { return status_text(app).find(timer_warning) != std::string::npos; };
    const auto edit_name = [&](const char *name) {
      auto *field = Access::name(app);
      field->value(name);
      app.field_changed(field);
    };
    expect(!warned() && Access::timer_fault_announcements(app) == 0,
           "healthy timers show no warning");

    // Every timer, the editor's own included, is refused while the fault lasts.
    queue.refuse(true);
    editor::steady_wake_fault_for_test(SteadyWakeFault::create);
    edit_name("Refused autosave edit");
    expect(Access::autosave_owed(app) && !Access::autosave_pending(app) && queue.size() == 0,
           "a refused autosave is owed with nothing queued");
    for (int pass = 0; pass < 3; ++pass) Fl::check();
    expect(warned() && Access::status_kind(app) == editor::StatusKind::warning &&
               Access::timer_fault_announcements(app) == 1,
           "the status column warns and the fault is spoken once");
    queue.advance(300ms);
    for (int pass = 0; pass < 3; ++pass) Fl::check();
    expect(layerc::test::read_file(fixture.path) == original && queue.size() == 0 &&
               app.session.has_unsaved_changes(),
           "loop passes before the steady deadline do not save");
    queue.advance(150ms);
    Fl::check();
    expect(queue.zero_calls() == 1 && layerc::test::read_file(fixture.path) == original,
           "past the deadline the retry posts one delivery and still does not save on the check");
    queue.deliver_zero();
    const auto saved = layerc::test::read_file(fixture.path);
    expect(saved && saved->find("Refused autosave edit") != std::string::npos &&
               !Access::autosave_owed(app) && !app.session.has_unsaved_changes(),
           "the delivered autosave saves once");
    expect(warned(), "the warning stays until a timer arms again");

    edit_name("Second refused edit");
    queue.advance(450ms);
    for (int pass = 0; pass < 5; ++pass) Fl::check();
    queue.deliver_zero();
    for (int pass = 0; pass < 5; ++pass) Fl::check();
    const auto second = layerc::test::read_file(fixture.path);
    expect(second && second->find("Second refused edit") != std::string::npos &&
               Access::timer_fault_announcements(app) == 1 && warned(),
           "a sustained fault across a second save is spoken once, without a report loop");

    app.set_capture_exclusion_warning("Capture exclusion fixture warning.");
    const auto both = status_text(app);
    const auto capture_at = both.find("Capture exclusion fixture warning.");
    const auto timer_at = both.find(timer_warning);
    expect(capture_at != std::string::npos && timer_at != std::string::npos &&
               capture_at < timer_at && Access::status_kind(app) == editor::StatusKind::warning,
           "the capture warning and the timer warning both show, capture first");
    app.report(editor::StatusKind::failure, "Fixture failure.");
    expect(Access::status_kind(app) == editor::StatusKind::failure &&
               status_text(app).find("Fixture failure.") != std::string::npos && warned(),
           "a failure keeps its severity beside the warnings");
    app.set_capture_exclusion_warning({});

    queue.refuse(false);
    editor::steady_wake_fault_for_test(SteadyWakeFault::none);
    edit_name("Recovered edit");
    for (int pass = 0; pass < 3; ++pass) Fl::check();
    expect(!editor::steady_timers_failing() && !warned() && Access::autosave_pending(app) &&
               Access::timer_fault_announcements(app) == 1,
           "the first armed timer clears the warning without another announcement");
    app.main_window()->do_callback();
    const auto closed = layerc::test::read_file(fixture.path);
    expect(closed && closed->find("Recovered edit") != std::string::npos,
           "close still flushes the recovered edit");
  }
  restore_healthy_timers();
}

// E (teardown): a queued report or announcement never reaches a destroyed editor.
void editor_drops_queued_timer_reports_on_teardown() {
  Fixture fixture;
  fixture.store();
  RefusingQueue queue;
  // The editor lives in storage that outlasts it, so its address can still be
  // compared with FLTK's queue after destruction.
  alignas(editor::EditorApp) std::byte storage[sizeof(editor::EditorApp)];
  {
    auto *app = ::new (static_cast<void *>(storage)) editor::EditorApp(
        Access::with_timer_scheduler(RefusingQueue::now, RefusingQueue::scheduler()));
    app->build();
    {
      Timer probe([] {}, RefusingQueue::now, RefusingQueue::scheduler());
      queue.refuse(true);
      probe.start(1s);
      Fl::check(); // delivers the report; the announcement waits for the next pass
      expect(Fl::has_timeout(Access::timer_fault_announcer(), app) != 0,
             "the announcement is deferred to a later pass");
    }
    app->~EditorApp();
  }
  expect(Fl::has_timeout(Access::timer_fault_announcer(), storage) == 0,
         "destruction withdraws the deferred announcement");
  {
    auto app = Access::with_timer_scheduler(RefusingQueue::now, RefusingQueue::scheduler());
    app.build();
    queue.refuse(false);
    Timer probe([] {}, RefusingQueue::now, RefusingQueue::scheduler());
    probe.start(1s); // clears the fault; its report is queued, not delivered
    probe.cancel();
  }
  Fl::check();
  Fl::check();
  expect(!editor::steady_timers_failing(), "no report reached a destroyed editor");
  restore_healthy_timers();
}

// F: a native modal loop dispatches OS timers without running either owner.
void steady_timers_wait_out_native_modal_loops() {
#ifdef _WIN32
  int scheduled{}, retried{};
  Clock::time_point scheduled_at{}, retried_at{};
  Timer first([&] {
    ++scheduled;
    scheduled_at = Clock::now();
  });
  Timer second([&] {
    ++retried;
    retried_at = Clock::now();
  });
  const auto start = Clock::now();
  first.start(50ms);
  editor::steady_wake_fault_for_test(SteadyWakeFault::create);
  second.start(50ms);
  editor::steady_wake_fault_for_test(SteadyWakeFault::none);
  expect(first.pending() && second.owed() && !second.pending(),
         "one timeout waits on the OS timer and one is owed for retry");
  bool ran_in_native_loop{};
  while (Clock::now() < start + 200ms) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    ran_in_native_loop = ran_in_native_loop || scheduled != 0 || retried != 0;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
  }
  const auto handler = Access::steady_timeout_handler();
  expect(!ran_in_native_loop && !editor::steady_wake_armed(handler, &first) &&
             Fl::has_timeout(handler, &first) && second.owed(),
         "the native loop only posted the first delivery to FLTK and ran neither owner");
  while ((scheduled == 0 || retried == 0) && Clock::now() < start + 3s) Fl::wait(0.05);
  for (int pass = 0; pass < 3; ++pass) Fl::wait(0.02);
  expect(scheduled == 1 && retried == 1 && scheduled_at >= start + 50ms &&
             retried_at >= start + 50ms && !first.owed() && !second.owed(),
         "back in FLTK both owners run once at or after their deadline");
  restore_healthy_timers();
#else
  std::cout << "Not applicable on Linux: this case exercises the Win32 native modal message loop.\n";
#endif
}
