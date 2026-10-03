#include "daemon_app.hpp"

#include <algorithm>
#include <cstddef>

namespace layerc::daemon {

using layerc::runtime::Capability;

DaemonApp::~DaemonApp() = default;
DaemonApp::DaemonApp() : runtime_kernel_(runtime_services()) {}

// One service per capability, each owning its logic; the kernel calls them
// directly.
layerc::runtime::RuntimeServiceRegistry DaemonApp::runtime_services() noexcept {
  layerc::runtime::RuntimeServiceRegistry services{};
  const auto serve = [&](const Capability capability,
                         layerc::runtime::IRuntimeService &service) {
    services[static_cast<std::size_t>(capability)] = &service;
  };
  serve(Capability::input, input_runtime_);
  serve(Capability::generic_hid, hid_runtime_);
  serve(Capability::overlay, overlay_runtime_);
  serve(Capability::visual, visual_service_);
  serve(Capability::isolated_visual, isolated_visual_service_);
  serve(Capability::automation, automation_runtime_host_);
  serve(Capability::gamebar, companion_host_);
  return services;
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
    const bool visual = visual_capture_runtime_.recognition_enabled && !session->is_global() &&
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
