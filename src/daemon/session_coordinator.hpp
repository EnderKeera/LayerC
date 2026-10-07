#pragma once

#include "daemon_state.hpp"

namespace layerc::daemon {

// Owns profile publication, module loading, session/foreground reconciliation,
// settings application, and the demand derived from those sessions. The shell
// receives notifications but never implements this policy.
class SessionCoordinator final : public SessionControl, public SessionQueries {
public:
  SessionCoordinator(SessionRegistry &sessions, LiveEditHost &live_edit,
                     OverlayRuntime &overlays, InputRuntime &input,
                     VisualCaptureRuntime &captures,
                     AutomationRuntimeHost &automation,
                     CompanionHost &companion,
                     RuntimeNotifications &notifications) noexcept
      : session_registry_(sessions), live_edit_host_(live_edit),
        overlay_runtime_(overlays), input_runtime_(input),
        visual_capture_runtime_(captures), automation_runtime_host_(automation),
        companion_host_(companion), notifications_(notifications) {}

  void reload_profiles() override;
  void apply_profile_update(std::string key, layerc::core::GameProfile profile,
                            layerc::core::ProfileRevision revision) override;
  void refresh_game_windows() override;
  [[nodiscard]] std::vector<Session *> foreground_sessions() const override;

  void sync_foreground_state();
  void load_settings();
  void reload_settings();
  [[nodiscard]] layerc::runtime::CapabilityDemand runtime_demand();
  [[nodiscard]] const std::string &settings_error() const noexcept {
    return settings_error_;
  }

private:
  void reconcile_profile_sources(std::vector<ProfileSource> loaded);

  SessionRegistry &session_registry_;
  LiveEditHost &live_edit_host_;
  OverlayRuntime &overlay_runtime_;
  InputRuntime &input_runtime_;
  VisualCaptureRuntime &visual_capture_runtime_;
  AutomationRuntimeHost &automation_runtime_host_;
  CompanionHost &companion_host_;
  RuntimeNotifications &notifications_;
  std::string settings_error_;
};

} // namespace layerc::daemon
