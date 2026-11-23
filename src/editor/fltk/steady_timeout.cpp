#include "steady_timeout.hpp"

#include <utility>

namespace layerc::editor {

SteadyTimeout::SteadyTimeout(std::function<void()> fire, const Now now,
                             const Scheduler scheduler)
    : fire_(std::move(fire)), now_(now), scheduler_(scheduler) {}

SteadyTimeout::~SteadyTimeout() { cancel(); }

void SteadyTimeout::arm(const Clock::duration delay) {
  const auto seconds = std::chrono::duration<double>(delay).count();
  scheduler_.add(seconds > 0.0 ? seconds : 0.0, handle, this);
}

void SteadyTimeout::start(const Clock::duration delay) {
  cancel();
  const auto clamped = delay > Clock::duration::zero() ? delay : Clock::duration::zero();
  deadline_ = now_() + clamped;
  pending_ = true;
  arm(clamped);
}

void SteadyTimeout::cancel() {
  if (!pending_) return;
  pending_ = false;
  scheduler_.remove(handle, this);
}

void SteadyTimeout::handle(void *data) {
  auto &self = *static_cast<SteadyTimeout *>(data);
  if (!self.pending_) return;
  // FLTK has dequeued this call. If the deadline is still ahead, wait out
  // the steady remainder instead of firing.
  const auto remaining = self.deadline_ - self.now_();
  if (remaining > Clock::duration::zero()) {
    self.arm(remaining);
    return;
  }
  self.pending_ = false;
  // The callback may restart or destroy this timeout, so it runs from a copy.
  const auto fire = self.fire_;
  if (fire) fire();
}

} // namespace layerc::editor
