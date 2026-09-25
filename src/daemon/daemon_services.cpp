#include "daemon_app.hpp"
#include "daemon_runtime_services.hpp"

#include <algorithm>
#include <array>

namespace layerc::daemon {

using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileResult;
using layerc::core::VisualRuntime;

ServiceReconcileResult
DaemonApp::reconcile_capability(const Capability capability,
                                const CapabilityDemand &demand) {
  switch (capability) {
  case Capability::input:
    reconcile_input_runtime();
    return {};
  case Capability::generic_hid:
    return reconcile_hid_runtime()
               ? ServiceReconcileResult{}
               : ServiceReconcileResult{
                     false, "Raw Input rejected a required HID usage"};
  case Capability::overlay:
    reconcile_overlay_runtime();
    return {};
  case Capability::visual:
    reconcile_visual_captures(demand, VisualRuntime::inline_process);
    return {};
  case Capability::isolated_visual:
    reconcile_visual_captures(demand, VisualRuntime::isolated);
    return {};
  case Capability::automation:
    reconcile_automation_runtime();
    return {};
  case Capability::gamebar:
    return companion_host_.transport.start()
               ? ServiceReconcileResult{}
               : ServiceReconcileResult{false,
                                        "Game Bar transport could not start"};
  case Capability::count:
    break;
  }
  return {false, "unknown runtime capability"};
}

void DaemonApp::quiesce_capability(const Capability capability) noexcept {
  switch (capability) {
  case Capability::input:
    quiesce_input_runtime();
    return;
  case Capability::generic_hid:
    quiesce_hid_runtime();
    return;
  case Capability::overlay:
    quiesce_overlay_runtime();
    return;
  case Capability::visual:
    quiesce_visual_captures(VisualRuntime::inline_process);
    return;
  case Capability::isolated_visual:
    quiesce_visual_captures(VisualRuntime::isolated);
    return;
  case Capability::automation:
    quiesce_automation_runtime();
    return;
  case Capability::gamebar:
    companion_host_.transport.stop();
    companion_host_.set_active_session_count(0);
    return;
  case Capability::count:
    return;
  }
}

RuntimeResourceStats
DaemonApp::capability_resources(const Capability capability) const noexcept {
  switch (capability) {
  case Capability::input:
    return input_runtime_.resources();
  case Capability::generic_hid:
    return input_runtime_.hid_resources();
  case Capability::overlay:
    return overlay_runtime_.resources(session_registry_, live_edit_host_);
  case Capability::visual:
    return visual_capture_runtime_.resources(VisualRuntime::inline_process,
                                             session_registry_);
  case Capability::isolated_visual:
    return visual_capture_runtime_.resources(VisualRuntime::isolated,
                                             session_registry_);
  case Capability::automation:
    return automation_runtime_host_.resources(session_registry_);
  case Capability::gamebar:
    return companion_host_.resources();
  case Capability::count:
    break;
  }
  return {};
}

layerc::runtime::CapabilityDemand DaemonApp::runtime_demand() {
  const auto &active = session_registry_.active_inputs;
  const auto companion =
      layerc::gamebar::GameBarTransport::current_process_is_expected_package()
          ? current_companion_sessions()
          : std::vector<Session *>{};
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(session_registry_.sessions.size());
  for (const auto &owned : session_registry_.sessions) {
    const auto *session = owned.get();
    if (session == nullptr)
      continue;
    const bool visual = visual_recognition_enabled_ && !session->is_global() &&
                        session->game_window != nullptr &&
                        session->has_active_visual_rules();
    requests.push_back({
        .window = {reinterpret_cast<std::uintptr_t>(session->game_window)},
        .active = std::ranges::find(active, session) != active.end(),
        .global = session->is_global(),
        .visual = visual,
        .generic_hid = session->uses_hid,
        .isolated_visual = session->profile.visual_runtime ==
                           layerc::core::VisualRuntime::isolated,
        .automation = session->has_enabled_automations(),
        .companion = std::ranges::find(companion, session) != companion.end(),
        .profile_id = session->profile.id,
    });
  }
  return layerc::runtime::calculate_capability_demand(
      requests, live_edit_host_.session != nullptr);
}

void DaemonApp::reconcile_runtime_services() {
  runtime_kernel_.services().set_resource_ledger(
      {.handles = live_edit_host_.authenticated ? 1U : 0U});
  (void)runtime_kernel_.reconcile(runtime_demand());
}

} // namespace layerc::daemon
