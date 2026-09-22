#pragma once

#include "daemon_session.hpp"
#include "visual_capture_host.hpp"

#include "GameBarBridge.hpp"
#include "GameBarTransport.hpp"
#include "layerc/core/runtime_instances.hpp"
#include "layerc/platform/live_channel.hpp"
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

// Owns profile sources and the live sessions derived from them. At every stable
// reconciliation boundary, each non-global Session has exactly one matching
// RuntimeInstanceTracker key; active_inputs() contains only pointers owned by
// sessions().
class SessionRegistry final {
public:
  using Sources = std::vector<ProfileSource>;
  using Sessions = std::vector<std::unique_ptr<Session>>;
  using ActiveInputs = std::vector<Session *>;

  [[nodiscard]] Sources &sources() noexcept { return sources_; }
  [[nodiscard]] const Sources &sources() const noexcept { return sources_; }
  [[nodiscard]] Sessions &sessions() noexcept { return sessions_; }
  [[nodiscard]] const Sessions &sessions() const noexcept { return sessions_; }
  [[nodiscard]] ActiveInputs &active_inputs() noexcept { return active_inputs_; }
  [[nodiscard]] const ActiveInputs &active_inputs() const noexcept {
    return active_inputs_;
  }
  [[nodiscard]] layerc::core::RuntimeInstanceTracker &instances() noexcept {
    return instances_;
  }
  [[nodiscard]] const layerc::core::RuntimeInstanceTracker &instances() const
      noexcept {
    return instances_;
  }

  [[nodiscard]] HWND input_foreground() const noexcept {
    return input_foreground_;
  }
  void set_input_foreground(const HWND window) noexcept {
    input_foreground_ = window;
  }
  [[nodiscard]] HWND last_game_foreground() const noexcept {
    return last_game_foreground_;
  }
  void set_last_game_foreground(const HWND window) noexcept {
    last_game_foreground_ = window;
  }

  [[nodiscard]] bool invariant_holds() const noexcept;

private:
  Sources sources_;
  layerc::core::RuntimeInstanceTracker instances_;
  Sessions sessions_;
  ActiveInputs active_inputs_;
  HWND input_foreground_{};
  HWND last_game_foreground_{};
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

  [[nodiscard]] bool raw_input_registered() const noexcept {
    return raw_input_registered_;
  }
  void set_raw_input_registered(const bool value) noexcept {
    raw_input_registered_ = value;
  }
  [[nodiscard]] bool timer_running() const noexcept { return timer_running_; }
  void set_timer_running(const bool value) noexcept { timer_running_ = value; }

  [[nodiscard]] std::array<XINPUT_STATE, XUSER_MAX_COUNT> &controllers()
      noexcept {
    return controllers_;
  }
  [[nodiscard]] const std::array<XINPUT_STATE, XUSER_MAX_COUNT> &controllers()
      const noexcept {
    return controllers_;
  }
  void reset_controllers() noexcept { controllers_ = {}; }

  [[nodiscard]] HidRuntimeState *hid() noexcept { return hid_.get(); }
  [[nodiscard]] const HidRuntimeState *hid() const noexcept { return hid_.get(); }
  [[nodiscard]] HidRuntimeState &ensure_hid();
  void reset_hid() noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  hid_resources() const noexcept;

private:
  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controllers_{};
  std::unique_ptr<HidRuntimeState> hid_;
  bool raw_input_registered_{};
  bool timer_running_{};
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

  [[nodiscard]] Captures &captures() noexcept { return captures_; }
  [[nodiscard]] const Captures &captures() const noexcept { return captures_; }
  [[nodiscard]] HANDLE snapshot_event() const noexcept { return snapshot_event_; }
  [[nodiscard]] bool ensure_snapshot_event();
  void close_snapshot_event() noexcept;
  [[nodiscard]] bool open_activity_event();
  void publish_activity(bool active) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(layerc::core::VisualRuntime runtime,
            const SessionRegistry &sessions) const noexcept;
  [[nodiscard]] const std::wstring &status() const noexcept { return status_; }
  void set_status(std::wstring status) { status_ = std::move(status); }

private:
  Captures captures_;
  HANDLE activity_event_{};
  HANDLE snapshot_event_{};
  std::wstring status_{L"Visual recognition: inactive"};
};

// Owns the process-wide automation wake event, timer bookkeeping, trace
// subscription, and monotonically increasing origin sequence. Persistent
// automation state is written only for a Session with a live runtime.
class AutomationRuntimeHost final {
public:
  AutomationRuntimeHost() = default;
  ~AutomationRuntimeHost();
  AutomationRuntimeHost(const AutomationRuntimeHost &) = delete;
  AutomationRuntimeHost &operator=(const AutomationRuntimeHost &) = delete;

  [[nodiscard]] bool timer_running() const noexcept { return timer_running_; }
  void set_timer_running(const bool value) noexcept { timer_running_ = value; }
  [[nodiscard]] std::uint64_t next_origin() noexcept {
    return origin_sequence_++;
  }
  [[nodiscard]] bool trace_subscribed() const noexcept {
    return trace_subscribed_;
  }
  void set_trace_subscribed(const bool value) noexcept {
    trace_subscribed_ = value;
  }
  [[nodiscard]] HANDLE active_event() const noexcept { return active_event_; }
  void publish_activity(bool active) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(const SessionRegistry &sessions) const noexcept;

private:
  HANDLE active_event_{};
  std::uint64_t origin_sequence_{1};
  bool timer_running_{};
  bool trace_subscribed_{};
};

class OverlayEventSink {
public:
  virtual ~OverlayEventSink() = default;
  virtual void on_overlay_event(DWORD event, HWND window) = 0;
};

class LiveEditHost;

// Owns foreground/location hooks and the render timer state. WinEvent does not
// carry user data, so exactly one OverlayRuntime may own the process callback
// target at a time. start() asserts that invariant and refuses to displace a
// live owner; stop() clears ownership before unhooking callbacks.
class OverlayRuntime final {
public:
  explicit OverlayRuntime(OverlayEventSink &sink) noexcept : sink_(sink) {}
  ~OverlayRuntime();
  OverlayRuntime(const OverlayRuntime &) = delete;
  OverlayRuntime &operator=(const OverlayRuntime &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] bool timer_running() const noexcept { return timer_running_; }
  void set_timer_running(const bool value) noexcept { timer_running_ = value; }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(const SessionRegistry &sessions,
            const LiveEditHost &live_edit) const noexcept;

private:
  static void CALLBACK event_proc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                  LONG object_id, LONG child_id,
                                  DWORD event_thread, DWORD event_time);
  static OverlayRuntime *callback_target_;

  OverlayEventSink &sink_;
  HWINEVENTHOOK foreground_hook_{};
  HWINEVENTHOOK location_hook_{};
  bool timer_running_{};
};

// Owns the authenticated editor connection and its optional edit session. An
// EditSession is observable only while the channel peer is authenticated.
class LiveEditHost final {
public:
  [[nodiscard]] layerc::platform::NamedPipeEditorDaemonChannel &channel()
      noexcept {
    return channel_;
  }
  [[nodiscard]] const layerc::platform::NamedPipeEditorDaemonChannel &channel()
      const noexcept {
    return channel_;
  }
  [[nodiscard]] std::unique_ptr<EditSession> &session() noexcept {
    return session_;
  }
  [[nodiscard]] const std::unique_ptr<EditSession> &session() const noexcept {
    return session_;
  }
  [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }
  void set_authenticated(const bool value) noexcept { authenticated_ = value; }

private:
  layerc::platform::NamedPipeEditorDaemonChannel channel_{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server};
  std::unique_ptr<EditSession> session_;
  bool authenticated_{};
};

// Owns the bridge snapshot and same-package App Service transport. The bridge is
// cleared whenever the transport is unavailable or no eligible session exists.
class CompanionHost final {
public:
  [[nodiscard]] layerc::gamebar::GameBarBridge &bridge() noexcept {
    return bridge_;
  }
  [[nodiscard]] const layerc::gamebar::GameBarBridge &bridge() const noexcept {
    return bridge_;
  }
  [[nodiscard]] layerc::gamebar::GameBarTransport &transport() noexcept {
    return transport_;
  }
  [[nodiscard]] const layerc::gamebar::GameBarTransport &transport() const
      noexcept {
    return transport_;
  }
  void set_active_session_count(std::size_t count) noexcept;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats resources() const noexcept;

private:
  layerc::gamebar::GameBarBridge bridge_;
  layerc::gamebar::GameBarTransport transport_;
  std::uint32_t active_session_count_{};
};

} // namespace layerc::daemon
