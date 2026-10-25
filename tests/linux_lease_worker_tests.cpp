// LeaseWorker: one owned, idle-blocking worker with a mutex/condition pair and
// the shared client identity both lease protocols use.
#include "lease_worker.hpp"
#include "layerc/core/automation_trace_control.hpp"
#include "layerc/core/live_draft_control.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <optional>
#include <string>

namespace {

using layerc::editor::LeaseWorker;
using layerc::editor::make_lease_client_id;
using layerc::test::expect;

} // namespace

void lease_worker_contracts() {
  // submit() wakes an indefinite wait and the take step sees the new state.
  {
    LeaseWorker worker;
    std::optional<int> ready;
    int taken{};
    std::promise<void> done;
    auto finished = done.get_future();
    worker.start([&] {
      (void)worker.wait([&] { return ready.has_value(); }, std::nullopt,
                        [&] { taken = *ready; });
      done.set_value();
    });
    expect(worker.submit([&] { ready = 7; }), "submit is accepted before stop");
    expect(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
               taken == 7,
           "submit wakes the worker and the take step observes the new value");
    worker.stop();
  }
  // A false predicate with a deadline runs the take step at/after that deadline
  // without any further wakeup; the timeout is a lower bound, not a wall clock.
  {
    LeaseWorker worker;
    std::atomic<bool> taken{false};
    std::atomic<layerc::core::TimePoint> observed{};
    std::promise<void> done;
    auto finished = done.get_future();
    const auto deadline =
        layerc::core::MonotonicClock::now() + std::chrono::milliseconds(30);
    worker.start([&] {
      (void)worker.wait([&] { return false; }, deadline, [&] {
        observed = layerc::core::MonotonicClock::now();
        taken = true;
      });
      done.set_value();
    });
    expect(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
               taken.load(),
           "a bounded wait without a ready predicate still runs its take step");
    expect(taken.load() && observed.load() >= deadline,
           "the take step cannot run before its monotonic deadline");
    worker.stop();
  }
  // stop() interrupts an indefinite wait without running take, and is idempotent.
  {
    LeaseWorker worker;
    std::atomic<bool> taken{false};
    std::atomic<bool> returned{false};
    std::promise<void> entering;
    auto entered = entering.get_future();
    bool announced{}; // worker-only
    worker.start([&] {
      (void)worker.wait([&] {
        if (!announced) {
          announced = true;
          entering.set_value();
        }
        return false;
      }, std::nullopt, [&] { taken = true; });
      returned = true;
    });
    expect(entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
           "the worker reaches its indefinite wait before stop");
    worker.stop();
    worker.stop();
    expect(returned.load() && !taken.load(),
           "stop interrupts an indefinite wait without a take step, and repeats safely");
    expect(!worker.submit([] {}), "submit after stop is refused");
  }
  // Both lease protocols share one identity format.
  const auto first = make_lease_client_id();
  const auto second = make_lease_client_id();
  expect(first != second && first.find('-') != std::string::npos &&
             layerc::core::trace_control_client_bytes >= first.size() &&
             layerc::core::live_draft_client_bytes >= first.size(),
         "lease client ids are unique per client and fit both protocol bounds");
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(lease_worker_contracts))
