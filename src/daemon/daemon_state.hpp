#pragma once

#include "daemon_session.hpp"
#include "visual_capture_host.hpp"

#include "layerc/core/companion_snapshot.hpp"
#include "GameBarTransport.hpp"
#include "layerc/core/runtime_instances.hpp"
#include "layerc/platform/automation_persistence.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <xinput.h>

namespace layerc::daemon {

// Message-window timer ids shared by the daemon loop and its services.
inline constexpr UINT_PTR game_scan_timer = 1;
inline constexpr UINT_PTR input_poll_timer = 2;
inline constexpr UINT_PTR overlay_render_timer = 3;
inline constexpr UINT_PTR automation_timer = 4;

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

// The authenticated editor connection and its optional edit session. An
// EditSession is observable only while the channel peer is authenticated.
struct LiveEditHost final {
  layerc::platform::NamedPipeEditorDaemonChannel channel{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server};
  std::unique_ptr<EditSession> session;
  bool authenticated{};
};

// The runtime services below are the kernel's per-capability services. Each
// owns its capability's resources and reconcile/quiesce logic and holds only
// narrow references to the session registry and the message window; the
// window reference is read at call time because the window is created after
// the services.

// Keyboard/mouse Raw Input registration and the 16 ms poll timer, active only
// while current capability demand requires them.
class InputRuntime final : public layerc::runtime::IRuntimeService {
public:
  InputRuntime(SessionRegistry &sessions, const HWND &window) noexcept
      : sessions_(sessions), window_(window) {}
  InputRuntime(const InputRuntime &) = delete;
  InputRuntime &operator=(const InputRuntime &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void reset_controllers() noexcept { controllers = {}; }

  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controllers{};
  bool raw_keyboard_registered{};
  bool raw_mouse_registered{};
  bool timer_running{};

private:
  void set_raw_input(bool keyboard, bool mouse) noexcept;

  SessionRegistry &sessions_;
  const HWND &window_;
};

// Generic HID top-level usages and device enumeration. The HID state never
// outlives that demand; keyboard/mouse usages stay with InputRuntime so one
// process-wide RIDEV_REMOVE can never invalidate the other service.
class HidRuntime final : public layerc::runtime::IRuntimeService {
public:
  HidRuntime(SessionRegistry &sessions, const HWND &window) noexcept;
  ~HidRuntime() override;
  HidRuntime(const HidRuntime &) = delete;
  HidRuntime &operator=(const HidRuntime &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  [[nodiscard]] HidRuntimeState *state() noexcept { return state_.get(); }
  [[nodiscard]] const HidRuntimeState *state() const noexcept {
    return state_.get();
  }

private:
  SessionRegistry &sessions_;
  const HWND &window_;
  std::unique_ptr<HidRuntimeState> state_;
};

// Receives the overlay runtime's WinEvent notifications.
class OverlayEventSink {
public:
  virtual void on_overlay_event(DWORD event, HWND window) = 0;

protected:
  ~OverlayEventSink() = default;
};

// Owns foreground/location hooks, the render timer, and the dirty flag.
// WinEvent does not carry user data, so exactly one OverlayRuntime may own the
// process callback target at a time. start() asserts that invariant and
// refuses to displace a live owner; stop() clears ownership before unhooking.
class OverlayRuntime final : public layerc::runtime::IRuntimeService {
public:
  OverlayRuntime(OverlayEventSink &events, SessionRegistry &sessions,
                 LiveEditHost &live_edit, const HWND &window) noexcept
      : events_(events), sessions_(sessions), live_edit_(live_edit),
        window_(window) {}
  ~OverlayRuntime() override;
  OverlayRuntime(const OverlayRuntime &) = delete;
  OverlayRuntime &operator=(const OverlayRuntime &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void mark_dirty() noexcept { dirty_ = true; }
  [[nodiscard]] bool take_dirty() noexcept { return std::exchange(dirty_, false); }

  bool timer_running{};
  bool capture_exclusion{true};

private:
  static void CALLBACK event_proc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                  LONG object_id, LONG child_id,
                                  DWORD event_thread, DWORD event_time);
  static OverlayRuntime *callback_target_;

  OverlayEventSink &events_;
  SessionRegistry &sessions_;
  LiveEditHost &live_edit_;
  const HWND &window_;
  HWINEVENTHOOK foreground_hook_{};
  HWINEVENTHOOK location_hook_{};
  bool dirty_{};
};

// Owns all visual capture hosts and their wake handles, shared by the inline
// and isolated capture services. There is at most one host per HWND;
// snapshot_event() is open exactly while at least one host needs it, and the
// named activity event mirrors whether any host exists.
class VisualCaptureRuntime final {
public:
  using Captures =
      std::unordered_map<HWND, std::unique_ptr<VisualCaptureHost>>;

  VisualCaptureRuntime(SessionRegistry &sessions,
                       LiveEditHost &live_edit) noexcept
      : sessions_(sessions), live_edit_(live_edit) {}
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
  void sync_exclusions();
  // Recomputes the status line from current host snapshots and publishes a
  // change to the authenticated editor.
  void update_status();
  void publish_status(std::span<const VisualCaptureSnapshot> snapshots);
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources(layerc::core::VisualRuntime runtime) const noexcept;

  [[nodiscard]] SessionRegistry &sessions() noexcept { return sessions_; }

  Captures captures;
  std::wstring status{L"Visual recognition: inactive"};
  bool recognition_enabled{};

private:
  SessionRegistry &sessions_;
  LiveEditHost &live_edit_;
  layerc::platform::OwnedHandle activity_event_;
  layerc::platform::OwnedHandle snapshot_event_;
};

// The visual capability for one capture runtime (inline or isolated); both
// share the VisualCaptureRuntime host map.
class VisualCaptureService final : public layerc::runtime::IRuntimeService {
public:
  VisualCaptureService(VisualCaptureRuntime &captures,
                       const layerc::core::VisualRuntime runtime) noexcept
      : captures_(captures), runtime_(runtime) {}

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return captures_.resources(runtime_);
  }

private:
  VisualCaptureRuntime &captures_;
  layerc::core::VisualRuntime runtime_;
};

// Owns automation runtimes' process-wide wake event, timer, trace
// subscription, and the per-profile persistence that outlives sessions, plus
// every automation step: events, cooldown edges, trace publication, and
// persistent-value synchronization.
class AutomationRuntimeHost final : public layerc::runtime::IRuntimeService {
public:
  AutomationRuntimeHost(SessionRegistry &sessions, LiveEditHost &live_edit,
                        OverlayRuntime &overlays, const HWND &window) noexcept
      : sessions_(sessions), live_edit_(live_edit), overlays_(overlays),
        window_(window) {}
  ~AutomationRuntimeHost() override;
  AutomationRuntimeHost(const AutomationRuntimeHost &) = delete;
  AutomationRuntimeHost &operator=(const AutomationRuntimeHost &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void publish_activity(bool active) noexcept;
  void process_event(Session &session,
                     const layerc::core::AutomationEvent &event);
  void process_input_value(Session &session, std::string source, double value,
                           layerc::core::TimePoint now, bool momentary = false);
  void poll_cooldown_transitions(Session &session, layerc::core::TimePoint now,
                                 bool emit);
  // The one post-step glue: trace to the editor, then persistent values to
  // the owner and same-profile siblings, then the next timer deadline.
  void after_step(Session &session, layerc::core::TimePoint now);
  void poll_timer();
  // Stops one session's runtime and flushes its profile's persistent values.
  void retire_session(Session &session, layerc::core::TimePoint now);

  [[nodiscard]] layerc::platform::AutomationPersistence &persistence() noexcept {
    return persistence_;
  }
  [[nodiscard]] const layerc::platform::AutomationPersistence &
  persistence() const noexcept {
    return persistence_;
  }

  bool trace_subscribed{};

private:
  void schedule_timer();
  void publish_trace(Session &session);
  void synchronize_state(Session &session, layerc::core::TimePoint now);
  void acknowledge_saved(std::span<const std::filesystem::path> saved);

  SessionRegistry &sessions_;
  LiveEditHost &live_edit_;
  OverlayRuntime &overlays_;
  const HWND &window_;
  // Persistent automation values per profile path, outliving sessions.
  layerc::platform::AutomationPersistence persistence_;
  layerc::platform::OwnedHandle active_event_;
  bool timer_running_{};
};

// Owns the demand-driven same-package App Service transport.
class CompanionHost final : public layerc::runtime::IRuntimeService {
public:
  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void set_active_session_count(std::size_t count) noexcept;

  layerc::gamebar::GameBarTransport transport;

private:
  std::uint32_t active_session_count_{};
};

} // namespace layerc::daemon
