#pragma once

#include "layerc/core/types.hpp"

#include <poll.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace layerc::platform {

// Blocking poll adapter for the Linux foreground host: SIGINT/SIGTERM via
// signalfd plus one monotonic one-shot timerfd. The kernel timer carries the
// next deadline, so poll itself blocks indefinitely — there is no idle tick.
// Later phases register further descriptors (control socket, X connection).
class EventLoop {
public:
  struct Wake {
    bool stop_signal{};
    bool timer{};
    bool fd{};

    [[nodiscard]] bool any() const noexcept {
      return stop_signal || timer || fd;
    }
  };

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return signal_fd_ >= 0 && timer_fd_ >= 0;
  }

  // Extra descriptors reported in ready_fds(), including HUP/ERR. Re-register
  // to change read/write interest, only between poll_once calls.
  void register_fd(int fd, short events = POLLIN) noexcept;
  void unregister_fd(int fd) noexcept;

  // Arm for an absolute monotonic deadline (a past deadline fires
  // immediately); nullopt disarms so a deadline-less poll blocks until a
  // descriptor or signal wakes it.
  void set_deadline(std::optional<core::TimePoint> deadline) noexcept;

  // One poll iteration; every source that woke is reported, and a fired
  // timerfd is drained into last_expiry_count(). timeout_override_ms exists
  // for tests/probes (0 = nonblocking); the default blocks until a wake.
  [[nodiscard]] Wake
  poll_once(std::optional<int> timeout_override_ms = std::nullopt) noexcept;

  [[nodiscard]] const std::vector<int> &ready_fds() const noexcept {
    return ready_fds_;
  }
  [[nodiscard]] std::uint64_t last_expiry_count() const noexcept {
    return expiries_;
  }

private:
  int signal_fd_{-1};
  int timer_fd_{-1};
  std::uint64_t expiries_{};
  std::vector<::pollfd> extra_fds_;
  std::vector<int> ready_fds_;
  std::vector<::pollfd> poll_fds_;
};

} // namespace layerc::platform
