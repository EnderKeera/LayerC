#pragma once

#include <FL/Fl.H>

#include <chrono>
#include <functional>

namespace layerc::editor {

// A one-shot FLTK timeout whose due time is a steady_clock deadline.
//
// FLTK measures its timer queue with Fl::now (the wall clock on Win32 and
// X11), so a forward clock step can run a timeout early. The handler compares
// the steady clock with the deadline: an early call re-arms for the steady
// remainder without firing; a due call clears pending, then fires once. Only a
// pending timeout is queued, so the event loop still blocks while idle.
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
      Scheduler scheduler = {Fl::add_timeout, Fl::remove_timeout});
  ~SteadyTimeout(); // cancels
  SteadyTimeout(const SteadyTimeout &) = delete;
  SteadyTimeout &operator=(const SteadyTimeout &) = delete;

  // Replaces a pending timeout; a negative delay is due at once.
  void start(Clock::duration delay);
  void cancel();
  [[nodiscard]] bool pending() const noexcept { return pending_; }

private:
  static void handle(void *data);
  void arm(Clock::duration delay);

  std::function<void()> fire_;
  Now now_;
  Scheduler scheduler_;
  Clock::time_point deadline_{};
  bool pending_{};
};

} // namespace layerc::editor
