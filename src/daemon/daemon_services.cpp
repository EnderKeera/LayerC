#include "daemon_app.hpp"
#include "daemon_runtime_services.hpp"

#include <algorithm>
#include <array>

namespace layerc::daemon {

namespace {
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileResult;

DaemonApp &app_from(void *owner) noexcept {
  return *static_cast<DaemonApp *>(owner);
}

const DaemonApp &app_from(const void *owner) noexcept {
  return *static_cast<const DaemonApp *>(owner);
}
} // namespace

std::span<const CapabilityPolicy> DaemonApp::capability_policies() noexcept {
  // The single dispatch table. CapabilityService forwards each slot here and
  // calls the matching DaemonApp method — one hop, no identity wrappers.
  static constexpr std::array<CapabilityPolicy, layerc::runtime::capability_count>
      policies{{
          {Capability::input,
           [](void *owner, const CapabilityDemand &) {
             app_from(owner).reconcile_input_runtime();
             return ServiceReconcileResult{};
           },
           [](void *owner) noexcept { app_from(owner).quiesce_input_runtime(); },
           [](const void *owner) noexcept {
             return app_from(owner).input_runtime_.resources();
           }},
          {Capability::generic_hid,
           [](void *owner, const CapabilityDemand &) {
             return app_from(owner).reconcile_hid_runtime()
                        ? ServiceReconcileResult{}
                        : ServiceReconcileResult{
                              false, "Raw Input rejected a required HID usage"};
           },
           [](void *owner) noexcept { app_from(owner).quiesce_hid_runtime(); },
           [](const void *owner) noexcept {
             return app_from(owner).input_runtime_.hid_resources();
           }},
          {Capability::overlay,
           [](void *owner, const CapabilityDemand &) {
             app_from(owner).reconcile_overlay_runtime();
             return ServiceReconcileResult{};
           },
           [](void *owner) noexcept {
             app_from(owner).quiesce_overlay_runtime();
           },
           [](const void *owner) noexcept {
             return app_from(owner).overlay_runtime_.resources(
                 app_from(owner).session_registry_,
                 app_from(owner).live_edit_host_);
           }},
          {Capability::visual,
           [](void *owner, const CapabilityDemand &demand) {
             app_from(owner).reconcile_visual_captures(
                 demand, layerc::core::VisualRuntime::inline_process);
             return ServiceReconcileResult{};
           },
           [](void *owner) noexcept {
             app_from(owner).quiesce_visual_captures(
                 layerc::core::VisualRuntime::inline_process);
           },
           [](const void *owner) noexcept {
             return app_from(owner).visual_capture_runtime_.resources(
                 layerc::core::VisualRuntime::inline_process,
                 app_from(owner).session_registry_);
           }},
          {Capability::isolated_visual,
           [](void *owner, const CapabilityDemand &demand) {
             app_from(owner).reconcile_visual_captures(
                 demand, layerc::core::VisualRuntime::isolated);
             return ServiceReconcileResult{};
           },
           [](void *owner) noexcept {
             app_from(owner).quiesce_visual_captures(
                 layerc::core::VisualRuntime::isolated);
           },
           [](const void *owner) noexcept {
             return app_from(owner).visual_capture_runtime_.resources(
                 layerc::core::VisualRuntime::isolated,
                 app_from(owner).session_registry_);
           }},
          {Capability::automation,
           [](void *owner, const CapabilityDemand &) {
             app_from(owner).reconcile_automation_runtime();
             return ServiceReconcileResult{};
           },
           [](void *owner) noexcept {
             app_from(owner).quiesce_automation_runtime();
           },
           [](const void *owner) noexcept {
             return app_from(owner).automation_runtime_host_.resources(
                 app_from(owner).session_registry_);
           }},
          {Capability::gamebar,
           [](void *owner, const CapabilityDemand &) {
             return app_from(owner).companion_host_.transport().start()
                        ? ServiceReconcileResult{}
                        : ServiceReconcileResult{
                              false, "Game Bar transport could not start"};
           },
           [](void *owner) noexcept {
             app_from(owner).companion_host_.transport().stop();
             app_from(owner).companion_host_.set_active_session_count(0);
           },
           [](const void *owner) noexcept {
             return app_from(owner).companion_host_.resources();
           }},
      }};
  return policies;
}

layerc::runtime::CapabilityDemand DaemonApp::runtime_demand() {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(session_registry_.active_inputs().size());
  const bool companion_available =
      layerc::gamebar::GameBarTransport::current_process_is_expected_package();
  for (const auto *session : session_registry_.active_inputs()) {
    if (session == nullptr)
      continue;
    const bool visual = visual_recognition_enabled_ && !session->is_global() &&
                        session->game_window != nullptr &&
                        session->has_active_visual_rules();
    requests.push_back({
        .window = {reinterpret_cast<std::uintptr_t>(session->game_window)},
        .active = true,
        .global = session->is_global(),
        .visual = visual,
        .generic_hid = session->uses_hid,
        .isolated_visual = session->profile.visual_runtime ==
                           layerc::core::VisualRuntime::isolated,
        .profile_id = session->profile.id,
    });
  }
  auto demand = layerc::runtime::calculate_capability_demand(
      requests, live_edit_host_.session() != nullptr);
  const auto automation_consumers = static_cast<std::uint32_t>(
      std::ranges::count_if(session_registry_.sessions(), [](const auto &session) {
        return session != nullptr && session->has_enabled_automations();
      }));
  demand.set(layerc::runtime::Capability::automation, automation_consumers);
  demand.set(layerc::runtime::Capability::gamebar,
             companion_available ? static_cast<std::uint32_t>(
                                       current_companion_sessions().size())
                                 : 0U);
  return demand;
}

void DaemonApp::reconcile_runtime_services() {
  runtime_kernel_.services().set_resource_ledger(
      {.handles = live_edit_host_.authenticated() ? 1U : 0U});
  (void)runtime_kernel_.reconcile(runtime_demand());
}

} // namespace layerc::daemon
