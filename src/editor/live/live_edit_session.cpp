#include "live_edit_session.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::editor {
namespace {

using layerc::core::LiveMessage;
using layerc::core::LiveMessageType;

[[nodiscard]] layerc::core::EditSessionId next_session_id(const HWND target) {
  return (static_cast<std::uint64_t>(GetTickCount64()) << 16U) ^
         (static_cast<std::uint64_t>(GetCurrentProcessId()) << 1U) ^
         reinterpret_cast<std::uintptr_t>(target);
}

void stamp_revision(LiveMessage &message,
                    const layerc::core::ProfileRevision revision) {
  message.revision = revision;
}

} // namespace

PipeLiveEditTransport::PipeLiveEditTransport(
    std::wstring pipe_name, const DWORD expected_server_process_id)
    : channel_(layerc::platform::NamedPipeEditorDaemonChannel::Role::client,
               std::move(pipe_name), expected_server_process_id) {}

std::vector<layerc::core::LiveMessageResult> PipeLiveEditTransport::poll() {
  return channel_.poll();
}

bool PipeLiveEditTransport::take_connection_changed() {
  return channel_.take_connection_changed();
}

bool PipeLiveEditTransport::connected() const { return channel_.connected(); }

bool PipeLiveEditTransport::send(const LiveMessage &message) {
  return channel_.send(message);
}

LiveEditSession::LiveEditSession(std::unique_ptr<LiveEditTransport> transport,
                                 Clock clock, TargetUsable target_usable)
    : transport_(std::move(transport)), clock_(std::move(clock)),
      target_usable_(std::move(target_usable)) {}

LiveEditSession::~LiveEditSession() = default;

bool live_target_usable(const HWND target) {
  return target != nullptr && IsWindow(target) && !IsIconic(target);
}

bool LiveEditSession::connected() const { return transport_->connected(); }

bool LiveEditSession::send(const LiveMessage &message) {
  return transport_->send(message);
}

void LiveEditSession::retire_session() noexcept {
  // Without a confirmed off, LayerD keeps Direct drag until it sees the end or
  // the lease expires; either is over by the lease release bound.
  if (session_id_ != 0 && (armed_ || disarm_revision_ != 0))
    release_by_ = clock_() + static_cast<ULONGLONG>(layerc::core::live_edit_lease_release.count());
  armed_ = false;
  disarm_revision_ = 0;
  disarm_deadline_ = 0;
  session_id_ = 0;
  peer_revision_ = 0;
  target_ = nullptr;
  pending_target_ = nullptr;
  pending_deadline_ = 0;
  direct_drag_notice_shown_ = false;
  pending_live_profile_ = false;
  live_profile_due_ = 0;
}

void LiveEditSession::handle_message(
    const LiveMessage &message, LiveEditStep &step,
    const std::function<bool(std::string_view)> &panel_known) {
  if (message.type == LiveMessageType::acknowledgement) {
    if (!authenticated_) {
      const auto expected = layerc::core::package_identity("LayerD") + ";";
      if (!message.text.starts_with(expected)) {
        step.authentication_rejected = true;
        return;
      }
      authenticated_ = true;
      step.authenticated_now = true;
      return;
    }
    if (session_id_ == 0 || message.edit_session != session_id_) {
      step.forwarded.push_back(message);
      return;
    }
    if (message.text == layerc::core::live_direct_drag_off_ack) {
      // Only the newest off request's answer confirms: an on request may
      // have followed an older one.
      if (disarm_revision_ != 0 && message.revision == disarm_revision_) {
        disarm_revision_ = 0;
        disarm_deadline_ = 0;
        armed_ = false;
        step.direct_drag_off = true;
      }
      return;
    }
    step.session_acknowledged = true;
    return;
  }
  if (message.type == LiveMessageType::error) {
    if (session_id_ != 0 && message.edit_session == session_id_) {
      step.ended = LiveEditEnd{LiveEditEndReason::daemon_error, message.text};
      retire_session();
      return;
    }
    step.forwarded.push_back(message);
    return;
  }
  if (message.type == LiveMessageType::end_edit) {
    if (session_id_ != 0 && message.edit_session == session_id_) {
      step.ended = LiveEditEnd{LiveEditEndReason::daemon_ended, message.text};
      retire_session();
    }
    return;
  }
  if (message.type == LiveMessageType::anchor_changed) {
    if (session_id_ == 0 || message.edit_session != session_id_) return;
    if (layerc::core::revision_decision(message.revision, peer_revision_) !=
        layerc::core::RevisionDecision::accept)
      return;
    if (!std::isfinite(message.anchor_x) || !std::isfinite(message.anchor_y) ||
        message.anchor_x < 0.0 || message.anchor_x > 1.0 ||
        message.anchor_y < 0.0 || message.anchor_y > 1.0)
      return;
    if (panel_known && !panel_known(message.panel_id)) return;
    // Only an accepted anchor advances the peer revision.
    peer_revision_ = message.revision;
    step.anchors.push_back(LiveEditAnchor{message.panel_id, message.anchor_x,
                                          message.anchor_y, message.final});
    return;
  }
  if (message.type == LiveMessageType::set_direct_drag) {
    // Only LayerD's own end of Direct drag is meaningful in this direction;
    // it shares the anchor revision order, so a stale one is ignored.
    if (session_id_ == 0 || message.edit_session != session_id_ ||
        message.direct_drag)
      return;
    if (layerc::core::revision_decision(message.revision, peer_revision_) !=
        layerc::core::RevisionDecision::accept)
      return;
    peer_revision_ = message.revision;
    // LayerD has disarmed every bar; an off request still out is answered
    // separately and keeps its own confirmation.
    armed_ = false;
    step.direct_drag_ended = true;
    return;
  }
  step.forwarded.push_back(message);
}

LiveEditStep LiveEditSession::poll(
    const std::function<bool(std::string_view panel_id)> &panel_known) {
  LiveEditStep step;
  for (auto &result : transport_->poll()) {
    if (const auto *error =
            std::get_if<layerc::core::LiveProtocolError>(&result)) {
      step.protocol_errors.push_back(*error);
      continue;
    }
    handle_message(std::get<LiveMessage>(result), step, panel_known);
  }
  if (transport_->take_connection_changed()) {
    authenticated_ = false;
    // A reconnect can arrive inside one poll interval, so any session is
    // retired here: it was negotiated with a daemon this editor no longer has.
    if (session_id_ != 0) {
      step.ended = LiveEditEnd{LiveEditEndReason::connection_lost, {}};
      retire_session();
    }
    if (transport_->connected()) {
      step.authenticating = true;
      (void)send({.type = LiveMessageType::handshake,
                  .revision = revision_,
                  .text = layerc::core::package_identity("LayerE")});
    }
  }

  // An off request LayerD never confirmed: stop the session, so no heartbeat
  // keeps LayerD's lease alive while a bar may still take clicks.
  if (session_id_ != 0 && disarm_revision_ != 0 && clock_() >= disarm_deadline_) {
    step.ended = LiveEditEnd{LiveEditEndReason::direct_drag_unconfirmed, {}};
    (void)end(true);
  }

  if (pending_target_ != nullptr) {
    if (authenticated_) {
      step.begin_ready = pending_target_;
      pending_target_ = nullptr;
      pending_deadline_ = 0;
    } else if (clock_() >= pending_deadline_) {
      pending_target_ = nullptr;
      pending_deadline_ = 0;
      step.pending_expired = true;
    }
  }

  if (authenticated_ && session_id_ != 0 &&
      clock_() - last_heartbeat_send_ >= heartbeat_interval_ms) {
    last_heartbeat_send_ = clock_();
    (void)send({.type = LiveMessageType::heartbeat,
                .revision = revision_,
                .edit_session = session_id_});
    if (!target_usable_(target_)) {
      step.ended = LiveEditEnd{LiveEditEndReason::target_closed, {}};
      retire_session();
    }
  }

  if (pending_live_profile_ && clock_() >= live_profile_due_) {
    pending_live_profile_ = false;
    live_profile_due_ = 0;
    step.live_profile_due = true;
  }

  // Poll at frame rate only while a session exchanges drags and heartbeats;
  // an idle connection needs no more than the reconnect cadence.
  const UINT desired_interval =
      transport_->connected() && session_id_ != 0 ? 16U : 100U;
  if (desired_interval != timer_interval_) {
    timer_interval_ = desired_interval;
    step.timer_interval = timer_interval_;
  }
  return step;
}

LiveEditBegin LiveEditSession::request_begin(const HWND target,
                                             LiveEditBeginPayload payload,
                                             const ULONGLONG await_ms) {
  if (transport_->connected() && authenticated_) {
    pending_target_ = nullptr;
    pending_deadline_ = 0;
    target_ = target;
    session_id_ = next_session_id(target);
    peer_revision_ = 0;
    direct_drag_notice_shown_ = false;
    advance_revision();
    (void)send({.type = LiveMessageType::begin_edit,
                .revision = revision_,
                .edit_session = session_id_,
                .target_window = reinterpret_cast<std::uintptr_t>(target),
                .profile_key = std::move(payload.profile_key),
                .panel_id = std::move(payload.panel_id),
                .profile_json = std::move(payload.profile_json)});
    last_heartbeat_send_ = clock_();
    return LiveEditBegin::started;
  }
  pending_target_ = target;
  pending_deadline_ = clock_() + await_ms;
  return LiveEditBegin::awaiting_daemon;
}

std::optional<ULONGLONG> LiveEditSession::next_due() const {
  const ULONGLONG now = clock_();
  std::optional<ULONGLONG> due;
  const auto consider = [&](const ULONGLONG at) {
    const ULONGLONG wait = at > now ? at - now : 0;
    if (!due || wait < *due) due = wait;
  };
  if (pending_target_ != nullptr) consider(pending_deadline_);
  if (authenticated_ && session_id_ != 0)
    consider(last_heartbeat_send_ + heartbeat_interval_ms);
  if (pending_live_profile_) consider(live_profile_due_);
  if (session_id_ != 0 && disarm_revision_ != 0) consider(disarm_deadline_);
  return due;
}

bool LiveEditSession::end(const bool notify_daemon) {
  const bool was_active = session_id_ != 0;
  if (notify_daemon && was_active)
    (void)send({.type = LiveMessageType::end_edit,
                .revision = revision_,
                .edit_session = session_id_});
  retire_session();
  return was_active;
}

void LiveEditSession::request_live_profile() {
  if (session_id_ == 0 || pending_live_profile_) return;
  pending_live_profile_ = true;
  live_profile_due_ = clock_() + live_profile_interval_ms;
}

bool LiveEditSession::send_live_profile(std::string profile_json) {
  if (session_id_ == 0) return false;
  pending_live_profile_ = false;
  live_profile_due_ = 0;
  advance_revision();
  return send({.type = LiveMessageType::live_profile,
               .revision = revision_,
               .edit_session = session_id_,
               .profile_json = std::move(profile_json)});
}

bool LiveEditSession::send_anchor(const std::string_view panel_id,
                                  const double x, const double y,
                                  const bool final) {
  if (session_id_ == 0) return false;
  const ULONGLONG now = clock_();
  if (!final && now - last_drag_send_ < drag_throttle_ms) return false;
  last_drag_send_ = now;
  advance_revision();
  return send({.type = LiveMessageType::anchor_changed,
               .revision = revision_,
               .edit_session = session_id_,
               .panel_id = std::string(panel_id),
               .anchor_x = x,
               .anchor_y = y,
               .final = final});
}

bool LiveEditSession::send_direct_drag(const bool enabled) {
  if (session_id_ == 0) return false;
  advance_revision();
  if (!send({.type = LiveMessageType::set_direct_drag,
             .revision = revision_,
             .edit_session = session_id_,
             .direct_drag = enabled})) {
    // A lost request could leave bars taking clicks while heartbeats keep
    // LayerD's lease alive: stop the session instead. LayerD's state is now
    // unknown, so the lease wait applies.
    armed_ = true;
    (void)end(true);
    return false;
  }
  if (enabled) {
    armed_ = true;
    disarm_revision_ = 0;
    disarm_deadline_ = 0;
  } else {
    disarm_revision_ = revision_;
    disarm_deadline_ = clock_() + direct_drag_ack_ms;
  }
  return true;
}

bool LiveEditSession::publish_committed(std::string profile_key,
                                        std::string profile_json) {
  advance_revision();
  if (!transport_->connected() || !authenticated_) return false;
  return send({.type = LiveMessageType::committed_profile,
               .revision = revision_,
               .profile_key = std::move(profile_key),
               .profile_json = std::move(profile_json)});
}

bool LiveEditSession::send_authenticated(LiveMessage message) {
  if (!transport_->connected() || !authenticated_) return false;
  advance_revision();
  stamp_revision(message, revision_);
  return send(message);
}

bool LiveEditSession::send_in_session(LiveMessage message) {
  if (session_id_ == 0) return false;
  advance_revision();
  stamp_revision(message, revision_);
  message.edit_session = session_id_;
  return send(message);
}

} // namespace layerc::editor
