#pragma once

#include "daemon_session.hpp"
#include "daemon_runtime_ports.hpp"
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
#include <functional>
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
inline constexpr UINT_PTR companion_snapshot_timer = 5;

struct HidRuntimeState;
class HidRuntime;
class OverlayRuntime;
class AutomationRuntimeHost;
class CompanionHost;
class VisualCaptureRuntime;

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

};

// The authenticated editor connection and its optional edit session. An
// EditSession is observable only while the channel peer is authenticated.
class LiveEditHost final : public layerc::runtime::IRuntimeService {
public:
  // Control is mandatory for the daemon's lifetime, rather than an optional
  // game capability. The root starts/quiesces this owner explicitly and
  // accounts its resources in the kernel's external resource ledger.
  LiveEditHost(SessionRegistry &sessions, SessionControl &control,
               OverlayRuntime &overlays, VisualCaptureRuntime &captures,
               AutomationRuntimeHost &automation,
               RuntimeNotifications &notifications,
               RuntimeDiagnosticsSource &diagnostics,
               std::wstring pipe_name = layerc::platform::live_edit_pipe_name,
               DWORD expected_editor_process_id = 0)
      : sessions_(sessions), control_(control), overlays_(overlays),
        captures_(captures), automation_(automation),
        notifications_(notifications), diagnostics_(diagnostics),
        pipe_name_(std::move(pipe_name)),
        expected_editor_process_id_(expected_editor_process_id) {}
  ~LiveEditHost() override;
  LiveEditHost(const LiveEditHost &) = delete;
  LiveEditHost &operator=(const LiveEditHost &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void poll();
  void rebuild_overlays();
  void update_overlays();
  void end_session(std::string_view reason = {});
  void set_capture_exclusion(bool enabled);
  [[nodiscard]] bool send_authenticated(
      const layerc::core::LiveMessage &message);
  [[nodiscard]] const EditSession *edit_session() const noexcept {
    return session_.get();
  }
  [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }
  [[nodiscard]] bool connected() const noexcept {
    return channel_ != nullptr && channel_->connected();
  }
  [[nodiscard]] DWORD peer_process_id() const noexcept {
    return channel_ != nullptr ? channel_->peer_process_id() : 0;
  }
  [[nodiscard]] std::size_t wait_handles(std::span<HANDLE> output) const noexcept {
    return channel_ != nullptr ? channel_->wait_handles(output) : 0;
  }

private:
  friend struct LiveEditHostTestAccess;
  void handle_message(const layerc::core::LiveMessage &message);
  void handle_committed_profile(const layerc::core::LiveMessage &message);
  void handle_begin_edit(const layerc::core::LiveMessage &message);
  void send_reply(layerc::core::LiveMessageType type,
                  const layerc::core::LiveMessage &request,
                  std::string text = {});
  void begin_session(const layerc::core::LiveMessage &message,
                     layerc::core::GameProfile profile);
  void on_direct_anchor(std::string_view panel_id, double anchor_x,
                        double anchor_y, bool final);
  void on_direct_drag_lost();

  SessionRegistry &sessions_;
  SessionControl &control_;
  OverlayRuntime &overlays_;
  VisualCaptureRuntime &captures_;
  AutomationRuntimeHost &automation_;
  RuntimeNotifications &notifications_;
  RuntimeDiagnosticsSource &diagnostics_;
  std::wstring pipe_name_;
  // Zero in LayerD, which trusts only its sibling editor image; tests name
  // their own process instead.
  DWORD expected_editor_process_id_{};
  std::unique_ptr<layerc::platform::NamedPipeEditorDaemonChannel> channel_;
  std::unique_ptr<EditSession> session_;
  std::function<void(const layerc::core::LiveMessage &)> reply_observer_;
  bool authenticated_{};
  bool stopped_{true};
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
  InputRuntime(SessionRegistry &sessions, const HWND &window, HidRuntime &hid,
               AutomationRuntimeHost &automation, OverlayRuntime &overlays,
               RuntimeNotifications &notifications) noexcept
      : sessions_(sessions), window_(window), hid_(hid), automation_(automation),
        overlays_(overlays), notifications_(notifications) {}
  InputRuntime(const InputRuntime &) = delete;
  InputRuntime &operator=(const InputRuntime &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void handle_raw_input(HRAWINPUT handle);
  void poll_input_devices();
  void seed_controller_states();
  void reset_controllers() noexcept { controllers_ = {}; }
  void process_input(Session &session, const layerc::core::InputEvent &event);

private:
  void set_raw_input(bool keyboard, bool mouse) noexcept;
  void poll_global_keyboard(Session &session);
  void poll_controllers();
  void emit_input(const layerc::core::InputEvent &event);

  SessionRegistry &sessions_;
  const HWND &window_;
  HidRuntime &hid_;
  AutomationRuntimeHost &automation_;
  OverlayRuntime &overlays_;
  RuntimeNotifications &notifications_;
  std::array<XINPUT_STATE, XUSER_MAX_COUNT> controllers_{};
  bool raw_keyboard_registered{};
  bool raw_mouse_registered{};
  bool timer_running{};
};

// Generic HID top-level usages and device enumeration. The HID state never
// outlives that demand; keyboard/mouse usages stay with InputRuntime so one
// process-wide RIDEV_REMOVE can never invalidate the other service.
class HidRuntime final : public layerc::runtime::IRuntimeService {
public:
  HidRuntime(SessionRegistry &sessions, const HWND &window, InputRuntime &input,
             AutomationRuntimeHost &automation,
             RuntimeNotifications &notifications) noexcept;
  ~HidRuntime() override;
  HidRuntime(const HidRuntime &) = delete;
  HidRuntime &operator=(const HidRuntime &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void handle_input(const RAWINPUT &input);
  void handle_device_change(HANDLE device, bool arrived);

private:
  void process_sample(Session &session, std::size_t index,
                      const layerc::core::HidControlBinding &binding,
                      const layerc::core::HidControlSample &sample);

  SessionRegistry &sessions_;
  const HWND &window_;
  InputRuntime &input_;
  AutomationRuntimeHost &automation_;
  RuntimeNotifications &notifications_;
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
                 LiveEditHost &live_edit, const HWND &window,
                 SessionControl &control, CompanionHost &companion,
                 VisualCaptureRuntime &captures) noexcept
      : events_(events), sessions_(sessions), live_edit_(live_edit),
        window_(window), control_(control), companion_(companion),
        captures_(captures) {}
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
  void flush();
  void sync_location(HWND changed_window);
  [[nodiscard]] bool capture_exclusion() const noexcept {
    return capture_exclusion_;
  }
  void set_capture_exclusion(bool enabled) noexcept {
    capture_exclusion_ = enabled;
  }

private:
  static void CALLBACK event_proc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                  LONG object_id, LONG child_id,
                                  DWORD event_thread, DWORD event_time);
  static OverlayRuntime *callback_target_;
  [[nodiscard]] RECT target_rect() const;
  void render(layerc::core::TimePoint now);

  OverlayEventSink &events_;
  SessionRegistry &sessions_;
  LiveEditHost &live_edit_;
  const HWND &window_;
  SessionControl &control_;
  CompanionHost &companion_;
  VisualCaptureRuntime &captures_;
  HWINEVENTHOOK foreground_hook_{};
  HWINEVENTHOOK location_hook_{};
  bool dirty_{};
  bool active_{};
  bool timer_running_{};
  std::optional<layerc::core::Milliseconds> timer_delay_;
  bool capture_exclusion_{true};
};

// Owns all visual capture hosts and their wake handles, shared by the inline
// and isolated capture services. There is at most one host per HWND;
// snapshot_event() is open exactly while at least one host needs it, and the
// named activity event mirrors whether any host exists.
class VisualCaptureRuntime final : public layerc::runtime::IRuntimeService {
public:
  using Captures =
      std::unordered_map<HWND, std::unique_ptr<VisualCaptureHost>>;

  VisualCaptureRuntime(SessionRegistry &sessions, LiveEditHost &live_edit,
                       OverlayRuntime &overlays,
                       AutomationRuntimeHost &automation,
                       RuntimeNotifications &notifications) noexcept
      : sessions_(sessions), live_edit_(live_edit), overlays_(overlays),
        automation_(automation), notifications_(notifications) {}
  ~VisualCaptureRuntime() override;
  VisualCaptureRuntime(const VisualCaptureRuntime &) = delete;
  VisualCaptureRuntime &operator=(const VisualCaptureRuntime &) = delete;

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return resources(layerc::core::VisualRuntime::inline_process);
  }

  void poll();
  void maintain();
  void retry(bool quarantined_only = false);
  [[nodiscard]] bool needs_maintenance() const noexcept;
  [[nodiscard]] const Captures &captures() const noexcept { return captures_; }
  [[nodiscard]] const std::wstring &status() const noexcept { return status_; }
  [[nodiscard]] bool recognition_enabled() const noexcept {
    return recognition_enabled_;
  }
  void set_recognition_enabled(bool enabled) noexcept {
    recognition_enabled_ = enabled;
  }

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

private:
  friend class IsolatedVisualCaptureService;
  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile_runtime(const layerc::runtime::CapabilityDemand &demand,
                    layerc::core::VisualRuntime runtime);
  void quiesce_runtime(layerc::core::VisualRuntime runtime) noexcept;

  SessionRegistry &sessions_;
  LiveEditHost &live_edit_;
  OverlayRuntime &overlays_;
  AutomationRuntimeHost &automation_;
  RuntimeNotifications &notifications_;
  Captures captures_;
  std::wstring status_{L"Visual recognition: inactive"};
  bool recognition_enabled_{};
  layerc::platform::OwnedHandle activity_event_;
  layerc::platform::OwnedHandle snapshot_event_;
};

// A typed isolated-capability facet of the capture owner, not a callback into
// DaemonApp. Shared wake handles and per-window hosts stay with that owner;
// quiescing this facet never tears down inline captures.
class IsolatedVisualCaptureService final
    : public layerc::runtime::IRuntimeService {
public:
  explicit IsolatedVisualCaptureService(VisualCaptureRuntime &captures) noexcept
      : captures_(captures) {}

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override {
    return captures_.reconcile_runtime(
        demand, layerc::core::VisualRuntime::isolated);
  }
  void quiesce() noexcept override {
    captures_.quiesce_runtime(layerc::core::VisualRuntime::isolated);
  }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return captures_.resources(layerc::core::VisualRuntime::isolated);
  }

private:
  VisualCaptureRuntime &captures_;
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

  void set_trace_subscribed(bool enabled);
  [[nodiscard]] bool trace_subscribed() const noexcept {
    return trace_subscribed_;
  }
  [[nodiscard]] bool retry(std::string_view profile_key, std::string_view rule);

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
  bool trace_subscribed_{};
};

// Owns publication, foreground selection, preference validation/writes, and
// the demand-driven same-package App Service transport.
class CompanionHost final : public layerc::runtime::IRuntimeService {
public:
  CompanionHost(SessionRegistry &sessions, SessionQueries &queries,
                SessionControl &control, const HWND &window) noexcept
      : sessions_(sessions), queries_(queries), control_(control),
        window_(window) {}
  ~CompanionHost() override;
  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

  void publish_snapshot(layerc::core::TimePoint now);
  void poll();
  [[nodiscard]] std::vector<Session *> current_sessions();
  [[nodiscard]] HANDLE wait_handle() const noexcept {
    return transport_.wait_handle();
  }
  [[nodiscard]] bool connected() const { return transport_.connected(); }
  // A transport fault wins; otherwise a snapshot that shed rows or could not
  // be published at all stays visible until a later snapshot fits.
  [[nodiscard]] std::wstring last_error() const {
    auto error = transport_.last_error();
    return error.empty() ? snapshot_fault_ : error;
  }

private:
  friend struct CompanionHostTestAccess;
  [[nodiscard]] bool apply_preferences(std::string_view json);
  void set_active_session_count(std::size_t count) noexcept;
  void schedule_publication(std::optional<layerc::core::TimePoint> next,
                            layerc::core::TimePoint now) noexcept;

  SessionRegistry &sessions_;
  SessionQueries &queries_;
  SessionControl &control_;
  const HWND &window_;
  layerc::gamebar::GameBarTransport transport_;
  HWND last_game_foreground_{};
  std::uint32_t active_session_count_{};
  std::wstring snapshot_fault_;
  bool timer_running_{};
};

} // namespace layerc::daemon
