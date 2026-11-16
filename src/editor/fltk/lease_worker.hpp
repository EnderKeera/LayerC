#pragma once

#include "layerc/core/types.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace layerc::editor {

// "<pid>-<monotonic ticks>": unique per editor client, valid for both lease
// protocols.
[[nodiscard]] std::string make_lease_client_id();

// One owned, demand-created, idle-blocking worker thread with a mutex/condition
// pair guarding the owner's request/update state. Declare it LAST in the owner
// so it joins before that state dies.
class LeaseWorker final {
public:
  LeaseWorker() = default;
  ~LeaseWorker(); // stop()
  LeaseWorker(const LeaseWorker &) = delete;
  LeaseWorker &operator=(const LeaseWorker &) = delete;
  void start(std::function<void()> body);
  void stop() noexcept; // idempotent: mark stopping, wake, join
  template <typename F> bool submit(F &&mutate);          // under lock, then notify; false if stopping
  template <typename F> decltype(auto) locked(F &&read);  // under lock
  // Worker side: block until ready() (under lock), stop, or `until`; then run take() under lock.
  // Returns false once stopping (take() not run).
  template <typename Ready, typename Take>
  bool wait(Ready &&ready, std::optional<layerc::core::TimePoint> until, Take &&take);

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_{};
  std::thread thread_;
};

template <typename F> bool LeaseWorker::submit(F &&mutate) {
  {
    const std::lock_guard lock(mutex_);
    if (stopping_) return false;
    std::forward<F>(mutate)();
  }
  condition_.notify_one();
  return true;
}

template <typename F> decltype(auto) LeaseWorker::locked(F &&read) {
  static_assert(!std::is_reference_v<std::invoke_result_t<F>>,
                "lease state must cross the mutex as a value");
  const std::lock_guard lock(mutex_);
  return std::forward<F>(read)();
}

template <typename Ready, typename Take>
bool LeaseWorker::wait(Ready &&ready, const std::optional<layerc::core::TimePoint> until,
                       Take &&take) {
  std::unique_lock lock(mutex_);
  const auto wake = [&] { return stopping_ || ready(); };
  if (until) condition_.wait_until(lock, *until, wake);
  else condition_.wait(lock, wake);
  if (stopping_) return false;
  take();
  return true;
}

} // namespace layerc::editor
