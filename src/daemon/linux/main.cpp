// Linux foreground daemon entry point. Platform variants are confined to the
// X11Adapters seam so orchestration is identical with and without Xlib.
#include "daemon_host.hpp"
#include "event_loop.hpp"
#include "x11_adapters.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using layerc::core::MonotonicClock;
using layerc::core::TimePoint;

[[nodiscard]] bool prepare_profile_store() {
  const auto roots = layerc::platform::profile_store_roots();
  if (!layerc::platform::ensure_profile_store_roots(roots)) {
    std::fputs("layerd: the profile store roots could not be created\n",
               stderr);
    return false;
  }
  if (const auto error =
          layerc::platform::recover_profile_transactions(roots).failure;
      error.has_value()) {
    std::fprintf(stderr, "layerd: profile transaction recovery failed (%s)\n",
                 layerc::platform::profile_write_error_text(*error));
    return false;
  }
  return true;
}

// Every capability starts unavailable; the adapters and the demand-driven
// status cascade promote the rows this platform implements.
[[nodiscard]] std::vector<layerc::daemon::CapabilityStatus>
unsupported_capabilities() {
  std::vector<layerc::daemon::CapabilityStatus> capabilities;
  capabilities.reserve(layerc::runtime::capability_count);
  for (std::size_t index = 0; index < layerc::runtime::capability_count;
       ++index)
    capabilities.push_back(
         {static_cast<layerc::runtime::Capability>(index),
          layerc::daemon::CapabilityAvailability::unavailable,
          {"platform_unsupported"},
          {}});
  return capabilities;
}

void report(const std::vector<std::string> &diagnostics) {
  for (const auto &diagnostic : diagnostics)
    std::fprintf(stderr, "layerd: %s\n", diagnostic.c_str());
}

class LinuxDaemon {
public:
  LinuxDaemon() : kernel_(adapters_.runtime_services()) {}

  [[nodiscard]] int run() {
    if (!prepare_profile_store())
      return 1;
    if (!loop_.valid()) {
      std::fprintf(stderr, "layerc: event loop setup failed\n");
      return 1;
    }
    std::string error;
    std::string warning;
    if (!control_.start(error, warning)) {
      std::fprintf(stderr, "layerd: %s\n", error.c_str());
      return 1;
    }
    if (!warning.empty())
      std::fprintf(stderr, "layerd: %s\n", warning.c_str());
    control_.attach(loop_);

    status_.x11.reason = "platform_unsupported";
    status_.capabilities = unsupported_capabilities();
    adapters_.start(loop_, status_, diagnostics_);
    report(diagnostics_);

    while (!stop_requested_)
      step();

    control_.shutdown(loop_);
    kernel_.quiesce();
    adapters_.stop_sessions(MonotonicClock::now());
    std::fprintf(stderr, "layerd: stopped\n");
    return 0;
  }

private:
  void step() {
    while (profiles_dirty_ || sessions_dirty_)
      reload();

    // Queue-aware prepare phase: synchronous Xlib queries during
    // refresh/render can buffer events client-side without making the
    // socket readable. Drain buffered work first with a nonblocking poll
    // so the daemon never sleeps on already-queued events; otherwise block
    // until a descriptor, timer, or signal wakes the loop (no polling
    // timer — idle means fully blocked).
    const bool queued = adapters_.has_pending_events();
    const auto wake = queued ? loop_.poll_once(0) : loop_.poll_once();
    if (wake.stop_signal)
      stop_requested_ = true;
    if (wake.timer) {
      const auto now = MonotonicClock::now();
      control_.expire_clients(loop_, now);
      sessions_dirty_ = adapters_.render(now) || sessions_dirty_;
      arm_deadline(now);
    }
    // Check phase: socket readiness OR locally queued Xlib events both
    // route to the single adapter dispatcher.
    if (!wake.fd && !adapters_.has_pending_events())
      return;
    if (wake.fd)
      service_control();
    dispatch_input(wake.fd);
    arm_deadline(MonotonicClock::now());
  }

  // Window and device changes only rematch sessions; the store is re-read
  // on a control reload (explicit or import notification).
  void reload() {
    const bool reread = profiles_dirty_;
    if (reread) {
      std::string load_warning;
      profiles_ = layerc::daemon::load_profiles(load_warning);
      if (!load_warning.empty())
        std::fprintf(stderr, "layerd: %s\n", load_warning.c_str());
    }
    profiles_dirty_ = sessions_dirty_ = false;

    diagnostics_.clear();
    adapters_.refresh(profiles_, reread, kernel_, status_, diagnostics_);
    // Focus and window changes rematch often; log them only when the summary
    // or its diagnostics changed. A store re-read always logs.
    const std::pair summary{adapters_.session_count(), profiles_.size()};
    if (reread || summary != logged_summary_ ||
        diagnostics_ != logged_diagnostics_) {
      std::fprintf(stderr, "layerd: %zu session(s) from %zu profile(s)\n",
                   summary.first, summary.second);
      report(diagnostics_);
      logged_summary_ = summary;
      logged_diagnostics_ = diagnostics_;
    }
    const auto now = MonotonicClock::now();
    sessions_dirty_ = adapters_.render(now) || sessions_dirty_;
    arm_deadline(now);
  }

  void service_control() {
    adapters_.refresh_status(kernel_, status_);
    status_.sessions = adapters_.session_diagnostics();
    const auto outcome =
        control_.service(loop_, loop_.ready_fds(), kernel_, status_);
    if (!outcome.diagnostic.empty())
      std::fprintf(stderr, "layerd: %s\n", outcome.diagnostic.c_str());
    stop_requested_ = stop_requested_ || outcome.stop_requested;
    profiles_dirty_ = profiles_dirty_ || outcome.reload_requested;
  }

  void dispatch_input(const bool readable) {
    const auto dispatch = readable ? adapters_.dispatch(loop_.ready_fds())
                                   : adapters_.dispatch_pending();
    if (dispatch.windows_changed)
      sessions_dirty_ = true;
    if (dispatch.devices_changed) {
      sessions_dirty_ = true;
      std::fprintf(stderr, "layerd: input devices changed\n");
    }
    for (const auto &cooldown : dispatch.triggered)
      std::fprintf(stderr, "layerd: triggered %s\n", cooldown.c_str());
    if (dispatch.events > 0)
      sessions_dirty_ = adapters_.render(MonotonicClock::now()) ||
                        sessions_dirty_;
  }

  void arm_deadline(const TimePoint now) {
    loop_.set_deadline(layerc::core::earliest_deadline(
        adapters_.next_deadline(now), control_.next_deadline()));
  }

  // Declaration order is the historical construction order; members are
  // destroyed in reverse, adapters last.
  layerc::daemon::X11Adapters adapters_;
  layerc::runtime::DaemonKernel kernel_;
  layerc::platform::EventLoop loop_;
  layerc::daemon::ControlService control_;
  layerc::daemon::LinuxStatusSnapshot status_;
  std::vector<std::string> diagnostics_;
  std::optional<std::pair<std::size_t, std::size_t>> logged_summary_;
  std::vector<std::string> logged_diagnostics_;
  std::vector<layerc::platform::StoredProfile> profiles_;
  bool profiles_dirty_{true};
  bool sessions_dirty_{};
  bool stop_requested_{};
};

} // namespace

int main() {
  LinuxDaemon daemon;
  return daemon.run();
}
