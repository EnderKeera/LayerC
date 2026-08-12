#pragma once

#include "daemon_session.hpp"
#include "isolated_visual_worker.hpp"
#include "visual_capture_worker.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/hid.hpp"
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

class DaemonApp {
public:
  struct HidRuntimeState;
  DaemonApp();
  ~DaemonApp();
  int run(HINSTANCE instance, int show_command);

private:
  class InputRuntimeService;
  class LiveEditRuntimeService;
  class OverlayRuntimeService;
  class VisualRuntimeService;
  class TemplateRuntimeService;
  class IsolatedVisualRuntimeService;
  class HidRuntimeService;

  static constexpr UINT tray_message = WM_APP + 1;
  static constexpr UINT_PTR game_scan_timer = 1;
  static constexpr UINT_PTR input_poll_timer = 2;
  static constexpr UINT_PTR overlay_render_timer = 3;
  static constexpr UINT command_open_editor = 1001;
  static constexpr UINT command_reload = 1002;
  static constexpr UINT command_exit = 1003;
  static DaemonApp *foreground_hook_target_;
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
  void reconcile_profile_sources(std::vector<ProfileSource> loaded);
  void apply_profile_update(std::wstring key,
                            layerc::core::GameProfile profile,
                            layerc::core::ProfileRevision revision);
  [[nodiscard]] bool reload_profiles_if_requested();
  [[nodiscard]] bool reload_settings_if_requested();
  void refresh_game_windows();
  void sync_foreground_state();
  void sync_overlay_location(HWND changed_window);
  void handle_raw_input(HRAWINPUT handle);
  void handle_hid_input(const RAWINPUT &input);
  void handle_hid_device_change(HANDLE device, bool arrived);
  [[nodiscard]] bool reconcile_hid_runtime();
  void quiesce_hid_runtime() noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  hid_runtime_resources() const noexcept;
  void poll_input_devices();
  [[nodiscard]] bool poll_global_keyboard(Session &session);
  void seed_controller_states();
  void poll_controllers();
  void emit_input(const layerc::core::InputEvent &event);
  [[nodiscard]] bool process_input(Session &session,
                                   const layerc::core::InputEvent &event);
  void update_overlays();
  void reconcile_overlay_runtime();
  void quiesce_overlay_runtime() noexcept;
  void reconcile_visual_captures();
  void quiesce_inline_visual_captures() noexcept;
  void quiesce_isolated_visual_captures() noexcept;
  void quiesce_visual_captures() noexcept;
  void reconcile_runtime_services();
  [[nodiscard]] layerc::runtime::CapabilityDemand runtime_demand() const;
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_input_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_live_edit_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_overlay_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_visual_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_template_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_isolated_visual_runtime_service(void *context);
  [[nodiscard]] static std::unique_ptr<layerc::runtime::IRuntimeService>
  create_hid_runtime_service(void *context);
  void poll_visual_captures();
  void maintain_isolated_visual_captures();
  void retry_quarantined_visual_helpers();
  void update_visual_status();
  void publish_visual_status(
      std::span<const VisualCaptureSnapshot> snapshots);
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
  [[nodiscard]] std::vector<Session *> foreground_sessions();
  [[nodiscard]] RECT overlay_target() const;
  static void CALLBACK foreground_event_proc(HWINEVENTHOOK hook, DWORD event,
                                             HWND window, LONG object_id,
                                             LONG child_id, DWORD event_thread,
                                             DWORD event_time);

  HINSTANCE instance_{};
  HWND window_{};
  NOTIFYICONDATAW tray_{};
  std::vector<ProfileSource> profile_sources_;
  layerc::core::RuntimeInstanceTracker runtime_instances_;
  std::vector<std::unique_ptr<Session>> sessions_;
  std::vector<Session *> active_input_sessions_;
  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controller_states_{};
  HWND input_foreground_{};
  HANDLE reload_event_{};
  HANDLE settings_reload_event_{};
  HANDLE visual_capture_active_event_{};
  HANDLE visual_snapshot_event_{};
  bool capture_exclusion_{true};
  bool visual_recognition_enabled_{true};
  bool shutting_down_{};
  bool raw_input_registered_{};
  bool input_timer_running_{};
  bool overlay_timer_running_{};
  bool live_package_authenticated_{};
  std::unique_ptr<HidRuntimeState> hid_runtime_;
  HWINEVENTHOOK foreground_event_hook_{};
  HWINEVENTHOOK location_event_hook_{};
  layerc::platform::NamedPipeEditorDaemonChannel live_channel_{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server};
  std::unique_ptr<EditSession> edit_session_;
  std::unordered_map<HWND, std::unique_ptr<VisualCaptureWorker>>
      visual_captures_;
  std::unordered_map<HWND, std::unique_ptr<IsolatedVisualWorker>>
      isolated_visual_captures_;
  layerc::runtime::DaemonKernel runtime_kernel_;
  std::wstring visual_status_{L"Visual recognition: inactive"};
};

} // namespace layerc::daemon
