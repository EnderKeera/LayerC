#include "steady_timeout.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {

std::vector<SteadyTimeout *> retrying;
bool failing{};
bool reported_failing{};
void (*fault_handler)(void *){};
void *fault_data{};

void notify_timer_fault(void *) {
  reported_failing = failing;
  if (fault_handler) fault_handler(fault_data);
}

void set_timer_fault(const bool value) {
  if (failing == value) return;
  failing = value;
  if (!fault_handler) return;
  if (failing == reported_failing) Fl::remove_timeout(notify_timer_fault);
  else Fl::add_timeout(0.0, notify_timer_fault);
}

} // namespace

bool steady_timers_failing() noexcept { return failing; }

void set_steady_timer_fault_handler(void (*handler)(void *), void *data) noexcept {
  if (!handler && data && data != fault_data) return;
  Fl::remove_timeout(notify_timer_fault);
  fault_handler = handler;
  fault_data = handler ? data : nullptr;
  reported_failing = failing;
}

SteadyTimeout::SteadyTimeout(std::function<void()> fire, const Now now,
                             const Scheduler scheduler)
    : fire_(std::move(fire)), now_(now), scheduler_(scheduler) {}

SteadyTimeout::~SteadyTimeout() { cancel(); }

void SteadyTimeout::delist() {
  if (state_ != State::retrying) return;
  std::erase(retrying, this);
  if (retrying.empty()) Fl::remove_check(retry);
}

void SteadyTimeout::arm(const Clock::duration delay) {
  const auto seconds = std::max(std::chrono::duration<double>(delay).count(), 0.0);
  const bool scheduled = scheduler_.add(seconds, handle, this);
  if (scheduled) {
    delist();
    state_ = State::scheduled;
  } else if (state_ != State::retrying) {
    const bool first = retrying.empty();
    retrying.push_back(this);
    state_ = State::retrying;
    if (first) Fl::add_check(retry);
  }
  if (seconds > 0.0) set_timer_fault(!scheduled);
}

void SteadyTimeout::start(const Clock::duration delay) {
  if (pending()) {
    scheduler_.remove(handle, this);
    state_ = State::idle;
  }
  const auto clamped = std::max(delay, Clock::duration::zero());
  deadline_ = now_() + clamped;
  arm(clamped);
}

void SteadyTimeout::cancel() {
  const bool scheduled = pending();
  delist();
  state_ = State::idle;
  if (scheduled) scheduler_.remove(handle, this);
}

void SteadyTimeout::retry(void *) {
  const auto snapshot = retrying;
  for (auto *timer : snapshot)
    if (std::ranges::find(retrying, timer) != retrying.end())
      timer->arm(timer->deadline_ - timer->now_());
}

void SteadyTimeout::handle(void *data) {
  auto &self = *static_cast<SteadyTimeout *>(data);
  if (!self.pending()) return;
  self.state_ = State::idle;
  // FLTK has dequeued this call. If the deadline is still ahead, wait out
  // the steady remainder instead of firing.
  const auto remaining = self.deadline_ - self.now_();
  if (remaining > Clock::duration::zero()) {
    self.arm(remaining);
    return;
  }
  // The callback may restart or destroy this timeout, so it runs from a copy.
  const auto fire = self.fire_;
  if (fire) fire();
}

} // namespace layerc::editor
