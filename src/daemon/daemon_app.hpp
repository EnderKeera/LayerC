#pragma once

#include "daemon_session.hpp"
#include "daemon_state.hpp"
#include "isolated_visual_worker.hpp"
#include "visual_capture_worker.hpp"

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

// Composition root and message loop. Capability logic lives in the runtime
// services (daemon_state.hpp), which the kernel calls directly.
class DaemonApp final : private OverlayEventSink {
public:
  DaemonApp();
  ~DaemonApp();
  int run(HINSTANCE instance, int show_command);

private:
  static constexpr UINT tray_message = WM_APP + 1;
  static constexpr UINT command_open_editor = 1001;
  static constexpr UINT command_reload = 1002;
  static constexpr UINT command_exit = 1003;
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam);
  LRESULT handle_message(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam);
  LRESULT dispatch_message(HWND window, UINT message, WPARAM wparam,
                           LPARAM lparam);

  bool create_message_window(HINSTANCE instance);
  void add_tray_icon();
  void ensure_tray_icon();
  void remove_tray_icon();
  void show_tray_menu();
  void launch_editor();
  void reload_profiles();
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
  void process_hid_sample(
      Session &session, std::size_t index,
      const layerc::core::HidControlBinding &binding,
      const layerc::core::HidControlSample &sample);
  void handle_hid_device_change(HANDLE device, bool arrived);
  void poll_input_devices();
  void poll_global_keyboard(Session &session);
  void seed_controller_states();
  void poll_controllers();
  void emit_input(const layerc::core::InputEvent &event);
  void process_input(Session &session, const layerc::core::InputEvent &event);
  void flush_overlays();
  void render_overlays();
  void publish_companion_snapshots(layerc::core::TimePoint now);
  void poll_gamebar_transport();
  [[nodiscard]] bool apply_companion_preferences(std::string_view json);
  void reconcile_runtime_services();
  [[nodiscard]] layerc::runtime::CapabilityDemand runtime_demand();
  void poll_visual_captures();
  void maintain_visual_captures();
  void retry_quarantined_visual_captures();
  [[nodiscard]] std::string runtime_diagnostics_json() const;
  void poll_live_channel();
  void handle_live_message(const layerc::core::LiveMessage &message);
  void handle_committed_profile(const layerc::core::LiveMessage &message);
  void handle_begin_edit(const layerc::core::LiveMessage &message);
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
  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry
  runtime_services() noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  NOTIFYICONDATAW tray_{};
  SessionRegistry session_registry_;
  LiveEditHost live_edit_host_;
  OverlayRuntime overlay_runtime_{*this, session_registry_, live_edit_host_,
                                  window_};
  InputRuntime input_runtime_{session_registry_, window_};
  HidRuntime hid_runtime_{session_registry_, window_};
  VisualCaptureRuntime visual_capture_runtime_{session_registry_,
                                               live_edit_host_};
  VisualCaptureService visual_service_{
      visual_capture_runtime_, layerc::core::VisualRuntime::inline_process};
  VisualCaptureService isolated_visual_service_{
      visual_capture_runtime_, layerc::core::VisualRuntime::isolated};
  AutomationRuntimeHost automation_runtime_host_{
      session_registry_, live_edit_host_, overlay_runtime_, window_};
  CompanionHost companion_host_;
  [[nodiscard]] std::vector<Session *> current_companion_sessions();
  layerc::platform::OwnedHandle reload_event_;
  layerc::platform::OwnedHandle settings_reload_event_;
  std::string settings_error_;
  bool shutting_down_{};
  layerc::runtime::DaemonKernel runtime_kernel_;
};

} // namespace layerc::daemon
