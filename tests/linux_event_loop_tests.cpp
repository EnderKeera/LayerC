#include "daemon_host.hpp"

#include "event_loop.hpp"
#include "layerc/runtime/daemon_kernel.hpp"
#include "test_support.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using layerc::platform::EventLoop;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::IRuntimeService;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::RuntimeServiceRegistry;
using layerc::runtime::ServiceReconcileResult;

class NoopInputService final : public IRuntimeService {
public:
  [[nodiscard]] ServiceReconcileResult
  reconcile(const CapabilityDemand &) override {
    return {};
  }
  void quiesce() noexcept override {}
  [[nodiscard]] RuntimeResourceStats resources() const noexcept override {
    return {};
  }
};

} // namespace

void linux_event_loop_contracts() {
  using layerc::test::expect;
  bool passed = true;

  // Deadline wake fires near the monotonic deadline.
  {
    EventLoop loop;
    passed &= expect(loop.valid(), "event loop descriptors should be valid");
    const auto deadline =
        layerc::core::MonotonicClock::now() + std::chrono::milliseconds(40);
    loop.set_deadline(deadline);
    const auto begin = std::chrono::steady_clock::now();
    const auto wake = loop.poll_once();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    passed &= expect(wake.timer && loop.last_expiry_count() == 1 &&
                         elapsed >= std::chrono::milliseconds(35) &&
                         elapsed <= std::chrono::seconds(2),
                     "timerfd should wake once near the monotonic deadline");
  }

  // A late (already past) deadline fires and is drained/disarmed afterwards.
  {
    EventLoop loop;
    loop.set_deadline(layerc::core::MonotonicClock::now() -
                      std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    passed &= expect(loop.poll_once().timer,
                     "past deadline should fire immediately");
    passed &= expect(!loop.poll_once(0).any(),
                     "one-shot timer should stay quiet after the drain");
  }

  // Without a deadline a probe reports nothing: no idle spin by construction.
  {
    EventLoop loop;
    loop.set_deadline(std::nullopt);
    passed &= expect(!loop.poll_once(0).any(),
                     "disarmed loop should not report wakeups");
  }

  // SIGTERM wakes the loop as a graceful stop.
  {
    const ::pid_t pid = ::fork();
    if (pid == 0) {
      EventLoop loop;
      if (!loop.valid()) _exit(2);
      for (;;) {
        const auto wake = loop.poll_once();
        if (wake.stop_signal) _exit(0);
        if (wake.timer) _exit(3);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    passed &= expect(::kill(pid, SIGTERM) == 0, "SIGTERM should be deliverable");
    int status = 0;
    passed &= expect(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                         WEXITSTATUS(status) == 0,
                     "loop should stop gracefully on SIGTERM");
  }

  // 5. Simultaneous sources are all reported, duplicate registration is
  // ignored, and unregistering quiets the descriptor.
  {
    EventLoop loop;
    int pipefd[2] = {-1, -1};
    passed &= expect(::pipe(pipefd) == 0, "pipe for registered fd");
    passed &= expect(::write(pipefd[1], "x", 1) == 1, "pipe write");
    loop.register_fd(pipefd[0]);
    loop.register_fd(pipefd[0]);
    loop.set_deadline(layerc::core::MonotonicClock::now() -
                      std::chrono::milliseconds(1));
    const auto wake = loop.poll_once(250);
    passed &= expect(wake.timer && wake.fd &&
                         std::count(loop.ready_fds().begin(),
                                    loop.ready_fds().end(), pipefd[0]) == 1,
                     "timer and one registered fd should wake together");
    loop.unregister_fd(pipefd[0]);
    passed &= expect(!loop.poll_once(0).any(),
                     "unregistered fd should stay quiet");
    (void)::close(pipefd[0]);
    (void)::close(pipefd[1]);
  }

  // 6. Demanded-unavailable diagnostics: a served capability is covered, an
  // unserved one is reported by name.
  {
    CapabilityDemand demand;
    demand.add(Capability::input);
    NoopInputService input;
    RuntimeServiceRegistry services{};
    services[static_cast<std::size_t>(Capability::input)] = &input;
    layerc::runtime::DaemonKernel served_kernel(services);
    passed &= expect(served_kernel.reconcile(demand),
                      "served demand should reconcile");
    passed &= expect(layerc::daemon::unavailable_capabilities(
                         served_kernel.demand(), served_kernel.services())
                         .empty(),
                     "served input should not be reported unavailable");

    layerc::runtime::DaemonKernel unserved_kernel;
    passed &= expect(unserved_kernel.reconcile(demand) == false,
                     "unserved demand should not reconcile cleanly");
    const auto unavailable = layerc::daemon::unavailable_capabilities(
        unserved_kernel.demand(), unserved_kernel.services());
    passed &= expect(unavailable.size() == 1 && unavailable.front() == "input",
                     "unserved input should be the single diagnostic");
  }

  (void)passed;
}

int main() {
  return layerc::test::run(
      {{"linux_event_loop_contracts", linux_event_loop_contracts}});
}
