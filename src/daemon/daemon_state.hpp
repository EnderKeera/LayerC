#pragma once

#include "daemon_session.hpp"
#include "visual_capture_host.hpp"

#include "layerc/core/companion_snapshot.hpp"
#include "GameBarTransport.hpp"
#include "layerc/core/runtime_instances.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <xinput.h>

namespace layerc::daemon {

struct HidRuntimeState;

// Profile sources and the live sessions derived from them. At every stable
// reconciliation boundary, each non-global Session has exactly one matching
// RuntimeInstanceTracker key; active_inputs contains only pointers owned by
// sessions.
struct SessionRegistry final {
  using Sources = std::vector<ProfileSource>;
  using Sessions = std::vector<std::unique_ptr<Session>>;
  using ActiveInputs = std::vector<Session *>;

  [[nodiscard]] bool invariant_holds() const noexcept;

  Sources sources;
  layerc::core::RuntimeInstanceTracker instances;
  Sessions sessions;
  ActiveInputs active_inputs;
  HWND input_foreground{};
  HWND last_game_foreground{};
};

// Owns every process-wide input resource. Raw Input registration and the 16 ms
// poll timer are active only while current capability demand requires them; HID
// enumeration state never outlives that demand.
class InputRuntime final {
public:
  InputRuntime() = default;
  ~InputRuntime();
  InputRuntime(const InputRuntime &) = delete;
  InputRuntime &operator=(const InputRuntime &) = delete;

  void reset_controllers() noexcept { controllers = {}; }

  [[nodiscard]] HidRuntimeState *hid() noexcept { return hid_.get(); }
  [[nodiscard]] const HidRuntimeState *hid() const noexcept { return hid_.get(); }
  [[nodiscard]] HidRuntimeState &ensure_hid();
  void reset_hid() noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  hid_resources() const noexcept;

  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controllers{};
  bool raw_keyboard_registered{};
  bool raw_mouse_registered{};
  bool timer_running{};

private:
  std::unique_ptr<HidRuntimeState> hid_;
};

// Owns all visual capture hosts and their wake handles. There is at most one
// host per HWND; snapshot_event() is open exactly while at least one host needs
// it, and the named activity event mirrors whether any host exists.
class VisualCaptureRuntime final {
public:
  using Captures =
      std::unordered_map<HWND, std::unique_ptr<VisualCaptureHost>>;

  VisualCaptureRuntime() = default;
  ~VisualCaptureRuntime();
  VisualCaptureRuntime(const VisualCaptureRuntime &) = delete;
  VisualCaptureRuntime &operator=(const VisualCaptureRuntime &) = delete;

  [[nodiscard]] HANDLE snapshot_event() const noexcept {
    return snapshot_event_.get();
  }
  [[nodiscard]] bool ensure_snapshot_event();
  void close_snapshot_event() noexcept { snapshot_event_.reset(); }
  [[nodiscard]] bool open_activity_event();
  void publish_activity(bool active) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(layerc::core::VisualRuntime runtime,
            const SessionRegistry &sessions) const noexcept;

  Captures captures;
  std::wstring status{L"Visual recognition: inactive"};

private:
  layerc::platform::OwnedHandle activity_event_;
  layerc::platform::OwnedHandle snapshot_event_;
};

// Owns the process-wide automation wake event, timer bookkeeping, and trace
// subscription. Persistent automation state is written only for a Session with
// a live runtime.
class AutomationRuntimeHost final {
public:
  AutomationRuntimeHost() = default;
  ~AutomationRuntimeHost();
  AutomationRuntimeHost(const AutomationRuntimeHost &) = delete;
  AutomationRuntimeHost &operator=(const AutomationRuntimeHost &) = delete;

  void publish_activity(bool active) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(const SessionRegistry &sessions) const noexcept;

  bool timer_running{};
  bool trace_subscribed{};

private:
  layerc::platform::OwnedHandle active_event_;
};

class DaemonApp;
struct LiveEditHost;

// Owns foreground/location hooks and the render timer state. WinEvent does not
// carry user data, so exactly one OverlayRuntime may own the process callback
// target at a time. start() asserts that invariant and refuses to displace a
// live owner; stop() clears ownership before unhooking callbacks.
class OverlayRuntime final {
public:
  explicit OverlayRuntime(DaemonApp &app) noexcept : app_(app) {}
  ~OverlayRuntime();
  OverlayRuntime(const OverlayRuntime &) = delete;
  OverlayRuntime &operator=(const OverlayRuntime &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(const SessionRegistry &sessions,
            const LiveEditHost &live_edit) const noexcept;

  bool timer_running{};

private:
  static void CALLBACK event_proc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                  LONG object_id, LONG child_id,
                                  DWORD event_thread, DWORD event_time);
  static OverlayRuntime *callback_target_;

  DaemonApp &app_;
  HWINEVENTHOOK foreground_hook_{};
  HWINEVENTHOOK location_hook_{};
};

// The authenticated editor connection and its optional edit session. An
// EditSession is observable only while the channel peer is authenticated.
struct LiveEditHost final {
  layerc::platform::NamedPipeEditorDaemonChannel channel{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server};
  std::unique_ptr<EditSession> session;
  bool authenticated{};
};

// Owns the demand-driven same-package App Service transport.
class CompanionHost final {
public:
  void set_active_session_count(std::size_t count) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats resources() const noexcept;

  layerc::gamebar::GameBarTransport transport;

private:
  std::uint32_t active_session_count_{};
};

} // namespace layerc::daemon
