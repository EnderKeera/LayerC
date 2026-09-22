#pragma once

#include "daemon_session.hpp"
#include "daemon_state.hpp"
#include "daemon_runtime_services.hpp"
#include "isolated_visual_worker.hpp"
#include "visual_capture_worker.hpp"

#include "GameBarBridge.hpp"
#include "GameBarTransport.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/runtime_instances.hpp"
#include "layerc/core/types.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <shellapi.h>
#include <windows.h>
#include <xinput.h>

namespace layerc::daemon {

class DaemonApp : private OverlayEventSink {
public:
  DaemonApp();
  ~DaemonApp();
  int run(HINSTANCE instance, int show_command);

private:
  static constexpr UINT tray_message = WM_APP + 1;
  static constexpr UINT_PTR game_scan_timer = 1;
  static constexpr UINT_PTR input_poll_timer = 2;
  static constexpr UINT_PTR overlay_render_timer = 3;
  static constexpr UINT_PTR automation_timer = 4;
  static constexpr UINT command_open_editor = 1001;
  static constexpr UINT command_reload = 1002;
  static constexpr UINT command_exit = 1003;
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam);
  LRESULT handle_message(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam);

  bool create_message_window(HINSTANCE instance);
  bool register_input_devices();
  void unregister_input_devices() noexcept;
  void reconcile_input_runtime();
  void quiesce_input_runtime() noexcept;
  void add_tray_icon();
  void ensure_tray_icon();
  void remove_tray_icon();
  void show_tray_menu();
  void launch_editor();
  void reload_profiles();
  void load_visual_source(ProfileSource &source);
  void load_automation_source(ProfileSource &source);
  void load_companion_source(ProfileSource &source);
  void reconcile_profile_sources(std::vector<ProfileSource> loaded);
  void apply_profile_update(std::string key, layerc::core::GameProfile profile,
                            layerc::core::ProfileRevision revision);
  [[nodiscard]] bool reload_profiles_if_requested();
  [[nodiscard]] bool reload_settings_if_requested();
  void refresh_game_windows();
  void sync_foreground_state();
  void sync_overlay_location(HWND changed_window);
  void handle_raw_input(HRAWINPUT handle);
  void handle_hid_input(const RAWINPUT &input);
  [[nodiscard]] bool process_hid_sample(
      Session &session, std::size_t index,
      const layerc::core::HidControlBinding &binding,
      const layerc::core::HidControlSample &sample);
  void handle_hid_device_change(HANDLE device, bool arrived);
  [[nodiscard]] bool reconcile_hid_runtime();
  void quiesce_hid_runtime() noexcept;
  void poll_input_devices();
  [[nodiscard]] bool poll_global_keyboard(Session &session);
  void seed_controller_states();
  void poll_controllers();
  void emit_input(const layerc::core::InputEvent &event);
  [[nodiscard]] bool process_input(Session &session,
                                   const layerc::core::InputEvent &event);
  [[nodiscard]] bool process_automation_input_value(Session &session,
                                                    std::string source,
                                                    double value,
                                                    layerc::core::TimePoint now,
                                                    bool momentary = false);
  void update_overlays();
  void publish_companion_snapshots(layerc::core::TimePoint now);
  void poll_gamebar_transport();
  [[nodiscard]] bool apply_companion_preferences(std::string_view json);
  void reconcile_overlay_runtime();
  void quiesce_overlay_runtime() noexcept;
  void reconcile_visual_captures(
      const layerc::runtime::CapabilityDemand &demand,
      layerc::core::VisualRuntime runtime);
  void quiesce_visual_captures(layerc::core::VisualRuntime runtime) noexcept;
  void reconcile_automation_runtime();
  void quiesce_automation_runtime() noexcept;
  void retire_automation_session(Session &session, layerc::core::TimePoint now);
  void schedule_automation_timer();
  void poll_automation_timer();
  [[nodiscard]] bool
  poll_automation_cooldown_transitions(Session &session,
                                       layerc::core::TimePoint now, bool emit);
  [[nodiscard]] bool
  trigger_cooldown_with_automation(Session &session,
                                   std::string_view cooldown_id,
                                   layerc::core::TimePoint now);
  [[nodiscard]] bool
  process_automation_event(Session &session,
                           const layerc::core::AutomationEvent &event);
  [[nodiscard]] layerc::core::AutomationEvent
  automation_event(layerc::core::AutomationEventKind kind,
                   std::string source_id, layerc::core::TimePoint now,
                   std::uint64_t origin_token);
  [[nodiscard]] layerc::core::AutomationEvent
  automation_event(layerc::core::AutomationEventKind kind,
                   std::string source_id, layerc::core::TimePoint now);
  [[nodiscard]] bool validate_automation_effect(
      const Session &session,
      const layerc::core::AutomationEffect &effect) const;
  [[nodiscard]] bool commit_automation_effects(
      Session &session, std::span<const layerc::core::AutomationEffect> effects,
      layerc::core::TimePoint now, std::uint64_t origin_token);
  void acknowledge_automation_state(const ProfileSource &source);
  void synchronize_automation_state(Session &session,
                                    layerc::core::TimePoint now);
  void reconcile_runtime_services();
  [[nodiscard]] layerc::runtime::CapabilityDemand runtime_demand();
  void poll_visual_captures();
  void maintain_visual_captures();
  void retry_quarantined_visual_captures();
  void update_visual_status();
  void publish_visual_status(std::span<const VisualCaptureSnapshot> snapshots);
  [[nodiscard]] std::string runtime_diagnostics_json() const;
  void poll_live_channel();
  void handle_live_message(const layerc::core::LiveMessage &message);
  void send_live_reply(layerc::core::LiveMessageType type,
                       const layerc::core::LiveMessage &request,
                       std::string text = {});
  void begin_edit_session(const layerc::core::LiveMessage &message,
                          layerc::core::GameProfile profile);
  void rebuild_edit_overlays();
  void update_edit_overlays();
  void end_edit_session(std::string_view reason = {});
  void on_direct_anchor(std::string_view panel_id, double anchor_x,
                        double anchor_y, bool final);
  void update_tooltip(const std::wstring &status);
  void load_platform_settings();
  [[nodiscard]] std::vector<Session *> foreground_sessions() const;
  [[nodiscard]] RECT overlay_target() const;
  void on_overlay_event(DWORD event, HWND window) override;
  void sync_visual_exclusions();
  [[nodiscard]] static std::span<const CapabilityPolicy>
  capability_policies() noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  NOTIFYICONDATAW tray_{};
  SessionRegistry session_registry_;
  InputRuntime input_runtime_;
  VisualCaptureRuntime visual_capture_runtime_;
  AutomationRuntimeHost automation_runtime_host_;
  OverlayRuntime overlay_runtime_{*this};
  LiveEditHost live_edit_host_;
  CompanionHost companion_host_;
  [[nodiscard]] std::vector<Session *> current_companion_sessions();
  HANDLE reload_event_{};
  HANDLE settings_reload_event_{};
  bool capture_exclusion_{true};
  bool visual_recognition_enabled_{};
  std::string settings_error_;
  bool shutting_down_{};
  CapabilityServices runtime_services_;
  layerc::runtime::DaemonKernel runtime_kernel_;
};

} // namespace layerc::daemon
