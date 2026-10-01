#include "event_loop.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <algorithm>

namespace layerc::platform {

EventLoop::EventLoop() {
  ::sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  // Route SIGINT/SIGTERM to the signalfd instead of default disposition.
  ::sigprocmask(SIG_BLOCK, &mask, nullptr);
  signal_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
}

EventLoop::~EventLoop() {
  if (signal_fd_ >= 0) (void)::close(signal_fd_);
  if (timer_fd_ >= 0) (void)::close(timer_fd_);
}

void EventLoop::set_deadline(
    const std::optional<core::TimePoint> deadline) noexcept {
  ::itimerspec spec{};
  if (deadline.has_value()) {
    const auto delta = *deadline - core::MonotonicClock::now();
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
    spec.it_value.tv_sec = static_cast<time_t>(nanoseconds / 1'000'000'000);
    spec.it_value.tv_nsec = static_cast<long>(nanoseconds % 1'000'000'000);
    if (nanoseconds <= 0) {
      spec.it_value.tv_sec = 0;
      spec.it_value.tv_nsec = 1;
    }
  }
  (void)::timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void EventLoop::register_fd(const int fd, const short events) noexcept {
  const auto found = std::ranges::find(extra_fds_, fd, &::pollfd::fd);
  if (found == extra_fds_.end())
    extra_fds_.push_back({.fd = fd, .events = events, .revents = 0});
  else
    found->events = events;
}

void EventLoop::unregister_fd(const int fd) noexcept {
  std::erase_if(extra_fds_, [fd](const auto &watch) { return watch.fd == fd; });
}

EventLoop::Wake
EventLoop::poll_once(const std::optional<int> timeout_override_ms) noexcept {
  expiries_ = 0;
  ready_fds_.clear();
  poll_fds_.clear();
  poll_fds_.push_back({.fd = signal_fd_, .events = POLLIN, .revents = 0});
  poll_fds_.push_back({.fd = timer_fd_, .events = POLLIN, .revents = 0});
  for (const auto &watch : extra_fds_)
    poll_fds_.push_back(watch);

  Wake wake;
  const int ready =
      ::poll(poll_fds_.data(), static_cast<::nfds_t>(poll_fds_.size()),
             timeout_override_ms.value_or(-1));
  if (ready <= 0) return wake; // timeout or EINTR
  for (const auto &pfd : poll_fds_)
    if ((pfd.revents & (pfd.events | POLLHUP | POLLERR | POLLNVAL)) != 0)
      ready_fds_.push_back(pfd.fd);

  if (std::ranges::find(ready_fds_, signal_fd_) != ready_fds_.end()) {
    ::signalfd_siginfo info{};
    wake.stop_signal =
        ::read(signal_fd_, &info, sizeof(info)) == sizeof(info);
  }
  if (std::ranges::find(ready_fds_, timer_fd_) != ready_fds_.end()) {
    std::uint64_t expiries = 0;
    if (::read(timer_fd_, &expiries, sizeof(expiries)) == sizeof(expiries)) {
      expiries_ = expiries;
      wake.timer = true;
    }
  }
  wake.fd = std::ranges::any_of(ready_fds_, [&](const int fd) {
    return fd != signal_fd_ && fd != timer_fd_;
  });
  return wake;
}

} // namespace layerc::platform
