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
    void (*add)(double, Fl_Timeout_Handler, void *);
    void (*remove)(Fl_Timeout_Handler, void *);
  };

  explicit SteadyTimeout(
      std::function<void()> fire, Now now = &Clock::now,
      Scheduler scheduler = {steady_wake_add, steady_wake_remove});
  ~SteadyTimeout(); // cancels
  SteadyTimeout(const SteadyTimeout &) = delete;
  SteadyTimeout &operator=(const SteadyTimeout &) = delete;

  // Replaces a pending timeout; a negative delay is due at once.
  void start(Clock::duration delay);
  void cancel();
  [[nodiscard]] bool pending() const noexcept { return pending_; }

private:
  friend struct EditorAppTestAccess;
  static void handle(void *data);
  void arm(Clock::duration delay);

  std::function<void()> fire_;
  Now now_;
  Scheduler scheduler_;
  Clock::time_point deadline_{};
  bool pending_{};
};

} // namespace layerc::editor
