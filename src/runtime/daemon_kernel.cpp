#include "layerc/runtime/daemon_kernel.hpp"

#include <algorithm>

namespace layerc::runtime {

CapabilityDemand calculate_capability_demand(
    const std::span<const SessionCapabilityRequest> sessions,
    const bool editor_connected, const bool edit_overlay_active) noexcept {
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
    if (!session.active) continue;
    demand.add(Capability::input);
    demand.add(Capability::overlay);
    if (session.generic_hid) demand.add(Capability::generic_hid);
    if (session.companion) demand.add(Capability::gamebar);
    if (session.global || !session.visual) continue;
    const bool isolated = std::ranges::find(isolated_windows, session.window) !=
                          isolated_windows.end();
    if (isolated) {
      demand.add(Capability::isolated_visual);
    } else {
      demand.add(Capability::visual);
      if (session.templates) demand.add(Capability::template_evaluation);
    }
  }
  if (editor_connected) demand.add(Capability::live_edit);
  if (edit_overlay_active) demand.add(Capability::overlay);
  return demand;
}

bool DaemonKernel::reconcile(
    CapabilityDemand demand,
    const std::span<const RuntimeServiceDescriptor> descriptors,
    const core::TimePoint now) {
  if (reconciling_) {
    pending_ = PendingReconcile{.demand = std::move(demand),
                                .descriptors = {descriptors.begin(),
                                                descriptors.end()},
                                .now = now};
    return true;
  }

  reconciling_ = true;
  bool success = true;
  PendingReconcile current{.demand = std::move(demand),
                           .descriptors = {descriptors.begin(),
                                           descriptors.end()},
                           .now = now};
  do {
    if (demand_ != current.demand) {
      demand_ = current.demand;
      ++demand_generation_;
    }
    success = services_.reconcile(demand_, current.descriptors, current.now) &&
              success;
    if (!pending_) break;
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
