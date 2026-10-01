#include "layerc/runtime/daemon_kernel.hpp"

#include <algorithm>
#include <vector>

namespace layerc::runtime {

CapabilityDemand calculate_capability_demand(
    const std::span<const SessionCapabilityRequest> sessions,
    const bool edit_overlay_active) {
  CapabilityDemand demand;
  std::vector<WindowId> isolated_windows;
  for (const auto &session : sessions) {
    if (!session.active || session.global || !session.visual ||
        !session.isolated_visual)
      continue;
    if (std::ranges::find(isolated_windows, session.window) ==
        isolated_windows.end())
      isolated_windows.push_back(session.window);
  }
  for (const auto &session : sessions) {
    // Automation timers and the companion snapshot follow a session while it
    // is in the background, so they count before the foreground filter.
    if (session.automation)
      demand.add(Capability::automation);
    if (session.companion)
      demand.add(Capability::gamebar);
    if (!session.active)
      continue;
    if (session.input)
      demand.add(Capability::input);
    if (session.overlay)
      demand.add(Capability::overlay);
    if (session.generic_hid)
      demand.add(Capability::generic_hid);
    if (session.global || !session.visual)
      continue;
    const bool isolated = std::ranges::find(isolated_windows, session.window) !=
                          isolated_windows.end();
    demand.add_visual_consumer(
        {session.window, session.profile_id,
         isolated ? core::VisualRuntime::isolated
                  : core::VisualRuntime::inline_process});
  }
  if (edit_overlay_active)
    demand.add(Capability::overlay);
  return demand;
}

DaemonKernel::DaemonKernel(const RuntimeServiceRegistry services) noexcept
    : services_(services) {}

bool DaemonKernel::reconcile(
    CapabilityDemand demand,
    const core::TimePoint now) {
  if (reconciling_) {
    pending_ = PendingReconcile{
        .demand = std::move(demand),
        .now = now};
    return true;
  }

  reconciling_ = true;
  bool success = true;
  PendingReconcile current{
      .demand = std::move(demand),
      .now = now};
  do {
    if (demand_ != current.demand) {
      demand_ = current.demand;
      ++demand_generation_;
    }
    success = services_.reconcile(demand_, current.now) && success;
    if (!pending_)
      break;
    current = std::move(*pending_);
    pending_.reset();
  } while (true);
  reconciling_ = false;
  return success;
}

void DaemonKernel::quiesce(const core::TimePoint now) noexcept {
  pending_.reset();
  services_.quiesce_all(now);
  if (!demand_.empty()) {
    demand_ = {};
    ++demand_generation_;
  }
}

} // namespace layerc::runtime
