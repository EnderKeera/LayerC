// Linux foreground daemon entry point. Platform variants are confined to the
// X11Adapters seam so orchestration is identical with and without Xlib.
#include "daemon_host.hpp"
#include "event_loop.hpp"
#include "x11_adapters.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main() {
  using layerc::platform::EventLoop;
  layerc::daemon::X11Adapters adapters;
  layerc::runtime::DaemonKernel kernel(adapters.runtime_services());

  const auto roots = layerc::platform::profile_store_roots();
  if (!layerc::platform::ensure_profile_store_roots(roots) ||
      !layerc::platform::recover_profile_transactions(roots)) {
    std::fprintf(stderr,
                 "layerd: profile store setup or transaction recovery failed\n");
    return 1;
  }

  EventLoop loop;
  if (!loop.valid()) {
    std::fprintf(stderr, "layerc: event loop setup failed\n");
    return 1;
  }

  std::string error;
  std::string warning;
  layerc::daemon::ControlService control;
  if (!control.start(error, warning)) {
    std::fprintf(stderr, "layerd: %s\n", error.c_str());
    return 1;
  }
  if (!warning.empty())
    std::fprintf(stderr, "layerd: %s\n", warning.c_str());
  control.attach(loop);

  layerc::daemon::LinuxStatusSnapshot linux_status;
  linux_status.x11.reason = "platform_unsupported";
  linux_status.capabilities = {
      {layerc::runtime::Capability::input,
       layerc::daemon::CapabilityAvailability::unavailable,
       {"platform_unsupported"}},
      {layerc::runtime::Capability::overlay,
       layerc::daemon::CapabilityAvailability::unavailable,
       {"platform_unsupported"}},
      {layerc::runtime::Capability::automation,
       layerc::daemon::CapabilityAvailability::unavailable,
       {"platform_unsupported"}},
      {layerc::runtime::Capability::visual,
       layerc::daemon::CapabilityAvailability::unavailable,
       {"platform_unsupported"}},
      {layerc::runtime::Capability::isolated_visual,
       layerc::daemon::CapabilityAvailability::unavailable,
       {"platform_unsupported"}},
  };

  std::vector<std::string> diagnostics;
  adapters.start(loop, linux_status, diagnostics);
  for (const auto &diagnostic : diagnostics)
    std::fprintf(stderr, "layerd: %s\n", diagnostic.c_str());

  auto arm_deadline = [&](const layerc::core::TimePoint now) {
    auto deadline = adapters.next_deadline(now);
    const auto control_deadline = control.next_deadline();
    if (control_deadline.has_value() &&
        (!deadline.has_value() || *control_deadline < *deadline))
      deadline = control_deadline;
    loop.set_deadline(deadline);
  };

  std::vector<layerc::platform::StoredProfile> profiles;
  bool dirty = true;
  auto rebuild = [&]() {
    diagnostics.clear();
    adapters.refresh(profiles, kernel, linux_status, diagnostics);
    std::fprintf(stderr, "layerd: %zu session(s) from %zu profile(s)\n",
                 adapters.session_count(), profiles.size());
    for (const auto &diagnostic : diagnostics)
      std::fprintf(stderr, "layerd: %s\n", diagnostic.c_str());
    const auto now = layerc::core::MonotonicClock::now();
    adapters.render(now);
    arm_deadline(now);
  };

  bool stop_requested = false;
  bool reload_requested = false;
  for (;;) {
    if (dirty) {
      std::string load_warning;
      profiles = layerc::daemon::load_profiles(load_warning);
      if (!load_warning.empty())
        std::fprintf(stderr, "layerd: %s\n", load_warning.c_str());
      dirty = false;
      rebuild();
    }

    // Queue-aware prepare phase: synchronous Xlib queries during
    // refresh/render can buffer events client-side without making the
    // socket readable. Drain buffered work first with a nonblocking poll
    // so the daemon never sleeps on already-queued events; otherwise block
    // until a descriptor, timer, or signal wakes the loop (no polling
    // timer — idle means fully blocked).
    const bool queued = adapters.has_pending_events();
    const auto wake = queued ? loop.poll_once(0) : loop.poll_once();
    if (wake.stop_signal) stop_requested = true;
    if (wake.timer) {
      const auto now = layerc::core::MonotonicClock::now();
      control.expire_clients(loop, now);
      adapters.render(now);
      arm_deadline(now);
    }
    // Check phase: socket readiness OR locally queued Xlib events both
    // route to the single adapter dispatcher.
    const bool check_queued = !wake.fd && adapters.has_pending_events();
    if (wake.fd || check_queued) {
      std::string diagnostic;
      if (wake.fd) {
        const auto outcome =
            control.service(loop, loop.ready_fds(), kernel, linux_status);
        diagnostic = outcome.diagnostic;
        stop_requested = stop_requested || outcome.stop_requested;
        reload_requested = reload_requested || outcome.reload_requested;
      }
      if (!diagnostic.empty())
        std::fprintf(stderr, "layerd: %s\n", diagnostic.c_str());

      const auto dispatch = wake.fd ? adapters.dispatch(loop.ready_fds())
                                    : adapters.dispatch_pending();
      if (dispatch.windows_changed) dirty = true;
      if (dispatch.devices_changed) {
        dirty = true;
        std::fprintf(stderr, "layerd: input devices changed\n");
      }
      for (const auto &cooldown : dispatch.triggered)
        std::fprintf(stderr, "layerd: triggered %s\n", cooldown.c_str());
      if (dispatch.events > 0) {
        const auto now = layerc::core::MonotonicClock::now();
        adapters.render(now);
      }
      arm_deadline(layerc::core::MonotonicClock::now());
    }
    if (reload_requested) {
      reload_requested = false;
      dirty = true;
    }
    if (stop_requested) break;
  }

  control.shutdown(loop);
  kernel.quiesce();
  adapters.stop_sessions(layerc::core::MonotonicClock::now());
  std::fprintf(stderr, "layerd: stopped\n");
  return 0;
}
