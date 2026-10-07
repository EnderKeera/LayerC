#include "daemon_app.hpp"

#include <algorithm>
#include <cstddef>

namespace layerc::daemon {

using layerc::runtime::Capability;

DaemonApp::~DaemonApp() {
  // Keep every peer alive while stopping the graph, including startup failure
  // and exception paths that never reach the message loop's normal shutdown.
  stop_runtime_services();
  if (window_ != nullptr) {
    remove_tray_icon();
    DestroyWindow(window_);
    window_ = nullptr;
  }
}
DaemonApp::DaemonApp() : runtime_kernel_(runtime_services()) {}

void DaemonApp::stop_runtime_services() noexcept {
  if (shutting_down_)
    return;
  shutting_down_ = true;
  live_edit_host_.quiesce();
  runtime_kernel_.services().set_resource_ledger({});
  runtime_kernel_.quiesce();
  overlay_runtime_.stop();
}

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
  serve(Capability::visual, visual_capture_runtime_);
  serve(Capability::isolated_visual, isolated_visual_service_);
  serve(Capability::automation, automation_runtime_host_);
  serve(Capability::gamebar, companion_host_);
  return services;
}



void DaemonApp::reconcile_runtime_services() {
  if (shutting_down_)
    return;
  runtime_kernel_.services().set_resource_ledger(
      live_edit_host_.resources());
  (void)runtime_kernel_.reconcile(session_coordinator_.runtime_demand());
}

void DaemonApp::sync_foreground_state() {
  if (!shutting_down_)
    session_coordinator_.sync_foreground_state();
}

bool DaemonApp::reload_profiles_if_requested() {
  if (!reload_event_.valid() ||
      WaitForSingleObject(reload_event_.get(), 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(reload_event_.get());
  session_coordinator_.reload_profiles();
  return true;
}

bool DaemonApp::reload_settings_if_requested() {
  if (!settings_reload_event_.valid() ||
      WaitForSingleObject(settings_reload_event_.get(), 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(settings_reload_event_.get());
  session_coordinator_.reload_settings();
  return true;
}

} // namespace layerc::daemon
