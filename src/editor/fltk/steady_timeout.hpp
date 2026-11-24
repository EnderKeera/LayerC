#pragma once

#include "steady_wake.hpp"

#include <chrono>
#include <functional>

namespace layerc::editor {

// A one-shot FLTK timeout whose due time is a steady_clock deadline.
//
// The default scheduler waits on the OS monotonic clock and defers delivery
// to FLTK. An early call re-arms for the steady remainder without firing; a due
// call clears pending, then fires once. Nothing is armed while idle.
class SteadyTimeout final {
public:
  using Clock = std::chrono::steady_clock;
  using Now = Clock::time_point (*)();
  struct Scheduler {
    bool (*add)(double, Fl_Timeout_Handler, void *);
    void (*remove)(Fl_Timeout_Handler, void *);
  };

  explicit SteadyTimeout(
      std::function<void()> fire, Now now = &Clock::now,
      Scheduler scheduler = {steady_wake_add, steady_wake_remove});
  ~SteadyTimeout(); // cancels
  SteadyTimeout(const SteadyTimeout &) = delete;
  SteadyTimeout &operator=(const SteadyTimeout &) = delete;

  // Replaces an owed timeout; a negative delay is due at once.
  void start(Clock::duration delay);
  void cancel();
  [[nodiscard]] bool pending() const noexcept { return state_ == State::scheduled; }
  [[nodiscard]] bool owed() const noexcept { return state_ != State::idle; }

private:
  enum class State { idle, retrying, scheduled };
  friend struct EditorAppTestAccess;
  static void handle(void *data);
  static void retry(void *);
  void delist();
  void arm(Clock::duration delay);

  std::function<void()> fire_;
  Now now_;
  Scheduler scheduler_;
  Clock::time_point deadline_{};
  State state_{};
};

[[nodiscard]] bool steady_timers_failing() noexcept;
// The FLTK-thread listener receives deferred changes, not an initial replay.
// A null handler detaches; non-null data only detaches a matching listener.
void set_steady_timer_fault_handler(void (*handler)(void *), void *data) noexcept;

} // namespace layerc::editor
