#pragma once

#include "layerc/core/live_edit.hpp"
#include "layerc/platform/live_channel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::editor {

// The authenticated daemon connection as one session needs it: drain, one-shot
// connection changes, connection state and send. The product implementation
// owns the named pipe; tests substitute a scripted transport.
class LiveEditTransport {
public:
  virtual ~LiveEditTransport() = default;
  [[nodiscard]] virtual std::vector<layerc::core::LiveMessageResult> poll() = 0;
  [[nodiscard]] virtual bool take_connection_changed() = 0;
  [[nodiscard]] virtual bool connected() const = 0;
  [[nodiscard]] virtual bool send(const layerc::core::LiveMessage &message) = 0;
};

// The editor's client end of the LayerD live-edit named pipe. Tests name a
// private pipe and pin the expected server process instead of layerd.exe.
class PipeLiveEditTransport final : public LiveEditTransport {
public:
  explicit PipeLiveEditTransport(
      std::wstring pipe_name = layerc::platform::live_edit_pipe_name,
      DWORD expected_server_process_id = 0);
  [[nodiscard]] std::vector<layerc::core::LiveMessageResult> poll() override;
  [[nodiscard]] bool take_connection_changed() override;
  [[nodiscard]] bool connected() const override;
  [[nodiscard]] bool send(const layerc::core::LiveMessage &message) override;
  // The pipe's pending I/O events, for an owner that blocks between polls.
  [[nodiscard]] std::size_t wait_handles(std::span<HANDLE> output) const noexcept {
    return channel_.wait_handles(output);
  }
  // Why the last connection attempt or peer authentication failed.
  [[nodiscard]] const std::wstring &last_error() const noexcept {
    return channel_.last_error();
  }

private:
  // ~64 KiB of overlapped I/O buffers; only ever heap-allocated.
  layerc::platform::NamedPipeEditorDaemonChannel channel_;
};

// The editor's target predicate: a live target is a real, unminimized window.
[[nodiscard]] bool live_target_usable(HWND target);

// Why the daemon-side session is gone. Every reason maps to one status line in
// the window; the controller never mutates UI state itself.
enum class LiveEditEndReason : std::uint8_t {
  connection_lost,
  daemon_error,
  daemon_ended,
  target_closed,
};

struct LiveEditEnd {
  LiveEditEndReason reason{LiveEditEndReason::daemon_ended};
  // Daemon-supplied text for the error/end_edit messages; empty otherwise.
  std::string text;
};

// One accepted remote anchor, already validated and revision-ordered.
struct LiveEditAnchor {
  std::string panel_id;
  double x{};
  double y{};
  bool final{};
};

// What the controller needs to start one session; the window supplies the
// serialized document so serialization stays a UI-side decision.
struct LiveEditBeginPayload {
  std::string profile_key;
  std::string panel_id;
  std::string profile_json;
};

enum class LiveEditBegin : std::uint8_t {
  started,
  awaiting_daemon,
};

// Everything one poll produced, in the order the window should apply it.
struct LiveEditStep {
  // Messages this session does not interpret (runtime status, diagnostics
  // reply, automation trace, and acknowledgements or errors that answer a
  // request outside the edit session, matched by their echoed revision).
  std::vector<layerc::core::LiveMessage> forwarded;
  std::vector<layerc::core::LiveProtocolError> protocol_errors;
  std::optional<HWND> begin_ready;
  bool pending_expired{};
  // Set only on the poll that sent the handshake after a connection change.
  bool authenticating{};
  bool authenticated_now{};
  bool authentication_rejected{};
  bool session_acknowledged{};
  // Every accepted anchor in arrival order: a final followed by a new drag
  // in the same poll must still commit the final locally.
  std::vector<LiveEditAnchor> anchors;
  std::optional<LiveEditEnd> ended;
  bool live_profile_due{};
  // Present only when the poll cadence changed.
  std::optional<UINT> timer_interval;
};

// Owns the authenticated daemon connection and one optional on-window edit
// session: the handshake, session lifecycle, heartbeat, anchor ordering and
// the coalesced live-profile send. It performs the protocol sends itself and
// stamps revisions; it never touches windows or dialogs.
class LiveEditSession final {
public:
  using Clock = std::function<ULONGLONG()>;
  using TargetUsable = std::function<bool(HWND)>;

  explicit LiveEditSession(
      std::unique_ptr<LiveEditTransport> transport =
          std::make_unique<PipeLiveEditTransport>(),
      Clock clock = [] { return ::GetTickCount64(); },
      TargetUsable target_usable = live_target_usable);
  ~LiveEditSession();
  LiveEditSession(const LiveEditSession &) = delete;
  LiveEditSession &operator=(const LiveEditSession &) = delete;
  LiveEditSession(LiveEditSession &&) noexcept = default;
  LiveEditSession &operator=(LiveEditSession &&) noexcept = default;

  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }
  [[nodiscard]] bool editing() const noexcept { return session_id_ != 0; }
  [[nodiscard]] layerc::core::EditSessionId session_id() const noexcept {
    return session_id_;
  }
  [[nodiscard]] HWND target() const noexcept { return target_; }
  [[nodiscard]] layerc::core::ProfileRevision revision() const noexcept {
    return revision_;
  }
  [[nodiscard]] layerc::core::ProfileRevision peer_revision() const noexcept {
    return peer_revision_;
  }
  [[nodiscard]] UINT timer_interval() const noexcept { return timer_interval_; }
  [[nodiscard]] bool awaiting_target() const noexcept {
    return pending_target_ != nullptr;
  }
  [[nodiscard]] bool direct_drag_notice_needed() const noexcept {
    return !direct_drag_notice_shown_;
  }
  [[nodiscard]] bool live_profile_pending() const noexcept {
    return pending_live_profile_;
  }

  // The starting revision is the window's launch identity; set once.
  void seed_revision(layerc::core::ProfileRevision revision) noexcept {
    revision_ = revision;
  }

  [[nodiscard]] LiveEditStep
  poll(const std::function<bool(std::string_view panel_id)> &panel_known);
  [[nodiscard]] LiveEditBegin request_begin(HWND target,
                                            LiveEditBeginPayload payload,
                                            ULONGLONG await_ms);
  // Sends end_edit for an active session when asked, and always retires the
  // session, its pending target and any coalesced live-profile send.
  [[nodiscard]] bool end(bool notify_daemon);
  // Coalesces one live-profile send; the next poll reports live_profile_due.
  void request_live_profile();
  [[nodiscard]] bool send_live_profile(std::string profile_json);
  // False when a non-final anchor is throttled below one frame.
  [[nodiscard]] bool send_anchor(std::string_view panel_id, double x, double y,
                                 bool final);
  [[nodiscard]] bool send_direct_drag(bool enabled);
  void note_direct_drag_notice() noexcept { direct_drag_notice_shown_ = true; }
  // Always advances the revision; sends only over an authenticated connection.
  [[nodiscard]] bool publish_committed(std::string profile_key,
                                       std::string profile_json);
  [[nodiscard]] bool send_authenticated(layerc::core::LiveMessage message);
  [[nodiscard]] bool send_in_session(layerc::core::LiveMessage message);

  static constexpr ULONGLONG heartbeat_interval_ms = 250;
  static constexpr ULONGLONG drag_throttle_ms = 16;
  static constexpr ULONGLONG live_profile_interval_ms = 33;

private:
  friend struct LiveEditSessionTestAccess;

  void advance_revision() noexcept { ++revision_; }
  [[nodiscard]] bool send(const layerc::core::LiveMessage &message);
  void handle_message(const layerc::core::LiveMessage &message,
                      LiveEditStep &step,
                      const std::function<bool(std::string_view)> &panel_known);
  void retire_session() noexcept;

  std::unique_ptr<LiveEditTransport> transport_;
  Clock clock_;
  TargetUsable target_usable_;
  layerc::core::ProfileRevision revision_{};
  layerc::core::ProfileRevision peer_revision_{};
  layerc::core::EditSessionId session_id_{};
  HWND target_{};
  HWND pending_target_{};
  ULONGLONG pending_deadline_{};
  ULONGLONG last_drag_send_{};
  ULONGLONG last_heartbeat_send_{};
  ULONGLONG live_profile_due_{};
  UINT timer_interval_{100};
  bool authenticated_{};
  bool direct_drag_notice_shown_{};
  bool pending_live_profile_{};
};

} // namespace layerc::editor
