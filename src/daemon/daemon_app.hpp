#pragma once

#include "daemon_state.hpp"
#include "session_coordinator.hpp"

#include "layerc/platform/win32_handle.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <string>

#include <shellapi.h>
#include <windows.h>

namespace layerc::daemon {

// Composition root and message loop. Capability logic lives in the runtime
// services (daemon_state.hpp), which the kernel calls directly.
class DaemonApp final : private OverlayEventSink,
                        private RuntimeNotifications,
                        private RuntimeDiagnosticsSource {
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
  [[nodiscard]] bool reload_profiles_if_requested();
  [[nodiscard]] bool reload_settings_if_requested();
  void sync_foreground_state() override;
  void reconcile_runtime_services() override;
  void stop_runtime_services() noexcept;
  [[nodiscard]] std::string runtime_diagnostics_json() const override;
  void update_tooltip(const std::wstring &status) override;
  void on_overlay_event(DWORD event, HWND window) override;
  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry
  runtime_services() noexcept;

  HINSTANCE instance_{};
  HWND window_{};
  NOTIFYICONDATAW tray_{};
  SessionRegistry session_registry_;
  // Construct the policy ports before converting them to base references in
  // the services below. Its concrete peer references are only bound here.
  SessionCoordinator session_coordinator_{
      session_registry_, live_edit_host_, overlay_runtime_, input_runtime_,
      visual_capture_runtime_, automation_runtime_host_, companion_host_, *this};
  LiveEditHost live_edit_host_{
      session_registry_, session_coordinator_, overlay_runtime_, visual_capture_runtime_,
      automation_runtime_host_, *this, *this};
  OverlayRuntime overlay_runtime_{*this, session_registry_, live_edit_host_,
                                  window_, session_coordinator_, companion_host_,
                                  visual_capture_runtime_};
  // Constructors only bind these references; no service is used until the
  // complete service graph and message window exist.
  InputRuntime input_runtime_{session_registry_, window_, hid_runtime_,
                              automation_runtime_host_, overlay_runtime_,
                              *this};
  HidRuntime hid_runtime_{session_registry_, window_, input_runtime_,
                          automation_runtime_host_, *this};
  VisualCaptureRuntime visual_capture_runtime_{
      session_registry_, live_edit_host_, overlay_runtime_,
      automation_runtime_host_, *this};
  IsolatedVisualCaptureService isolated_visual_service_{visual_capture_runtime_};
  AutomationRuntimeHost automation_runtime_host_{
      session_registry_, live_edit_host_, overlay_runtime_, window_};
  CompanionHost companion_host_{session_registry_, session_coordinator_,
                               session_coordinator_, window_};
  layerc::platform::OwnedHandle reload_event_;
  layerc::platform::OwnedHandle settings_reload_event_;
  bool shutting_down_{};
  layerc::runtime::DaemonKernel runtime_kernel_;
};

} // namespace layerc::daemon
