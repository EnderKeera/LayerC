#include "daemon_state.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

namespace layerc::daemon {

LiveEditHost::~LiveEditHost() = default;

layerc::runtime::ServiceReconcileResult
LiveEditHost::reconcile(const layerc::runtime::CapabilityDemand &) {
  if (channel_ == nullptr) {
    channel_ = std::make_unique<layerc::platform::NamedPipeEditorDaemonChannel>(
        layerc::platform::NamedPipeEditorDaemonChannel::Role::server,
        pipe_name_);
    if (!channel_->last_error().empty()) {
      const auto reason = layerc::platform::wide_to_utf8(channel_->last_error());
      channel_.reset();
      return layerc::runtime::ServiceReconcileResult::failed(reason);
    }
  }
  stopped_ = false;
  return {};
}

void LiveEditHost::quiesce() noexcept {
  // Set this before ending the lease: shutdown must not ask the composition
  // root to reconcile a graph which is already being torn down.
  stopped_ = true;
  end_session();
  authenticated_ = false;
  automation_.set_trace_subscribed(false);
  // The channel destructor cancels and drains every pending overlapped I/O.
  channel_.reset();
}

layerc::runtime::RuntimeResourceStats LiveEditHost::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result{
      .handles = channel_ != nullptr ? channel_->resource_handle_count() : 0U};
  if (session_ != nullptr)
    result += {
        .windows = static_cast<std::uint32_t>(session_->overlays.window_count()),
        .objects = static_cast<std::uint32_t>(session_->overlays.object_count())};
  return result;
}

bool LiveEditHost::send_authenticated(
    const layerc::core::LiveMessage &message) {
  return authenticated_ && channel_ != nullptr && channel_->connected() &&
         channel_->send(message);
}

void LiveEditHost::set_capture_exclusion(const bool enabled) {
  if (session_ != nullptr)
    session_->overlays.set_capture_exclusion(enabled);
}

void LiveEditHost::send_reply(const layerc::core::LiveMessageType type,
                                const layerc::core::LiveMessage &request,
                                std::string text) {
  (void)channel_->send({.type = type,
                            .revision = request.revision,
                            .edit_session = request.edit_session,
                            .profile_key = request.profile_key,
                            .text = std::move(text)});
}

void LiveEditHost::poll() {
  if (stopped_ || channel_ == nullptr)
    return;
  auto results = channel_->poll();
  const bool connection_changed = channel_->take_connection_changed();
  if (connection_changed) {
    authenticated_ = false;
    if (!channel_->connected()) {
      automation_.set_trace_subscribed(false);
      end_session("editor disconnected");
    }
  }

  for (auto &result : results) {
    if (const auto *error =
            std::get_if<layerc::core::LiveProtocolError>(&result)) {
      layerc::core::LiveMessage request;
      send_reply(layerc::core::LiveMessageType::error, request,
                      error->message);
      continue;
    }
    handle_message(std::get<layerc::core::LiveMessage>(result));
  }

  if (session_ != nullptr &&
      layerc::core::MonotonicClock::now() - session_->last_heartbeat >
          std::chrono::seconds(1)) {
    end_session("edit lease expired");
  }
  update_overlays();
  notifications_.reconcile_runtime_services();
}

void LiveEditHost::handle_message(const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  if (message.type == LiveMessageType::handshake) {
    if (message.text != layerc::core::package_identity("LayerE")) {
      authenticated_ = false;
      channel_->disconnect_peer();
      return;
    }
    authenticated_ = true;
    send_reply(LiveMessageType::acknowledgement, message,
                    layerc::core::package_identity("LayerD") +
                        ";live-profile,mirrored-drag,direct-drag,lease-v1,"
                        "visual-status-v1,diagnostics-v1,automation-trace-v1");
    (void)channel_->send({
        .type = LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(captures_.status()),
    });
    return;
  }
  if (!authenticated_) {
    channel_->disconnect_peer();
    return;
  }
  if (message.type == LiveMessageType::diagnostics_request) {
    send_reply(LiveMessageType::diagnostics_reply, message,
                    diagnostics_.runtime_diagnostics_json());
    return;
  }
  if (message.type == LiveMessageType::retry_visual) {
    captures_.retry();
    notifications_.reconcile_runtime_services();
    send_reply(LiveMessageType::acknowledgement, message,
                    "visual runtime retry requested");
    return;
  }
  if (message.type == LiveMessageType::automation_trace_subscribe ||
      message.type == LiveMessageType::automation_trace_unsubscribe) {
    automation_.set_trace_subscribed(
        message.type == LiveMessageType::automation_trace_subscribe);
    send_reply(LiveMessageType::acknowledgement, message,
                    automation_.trace_subscribed()
                        ? "automation trace subscribed"
                        : "automation trace unsubscribed");
    return;
  }
  if (message.type == LiveMessageType::retry_automation) {
    const bool retried =
        automation_.retry(message.profile_key, message.text);
    send_reply(retried ? LiveMessageType::acknowledgement
                            : LiveMessageType::error,
                    message,
                    retried ? "automation retry requested"
                            : "matching automation session was not found");
    return;
  }
  if (message.type == LiveMessageType::committed_profile) {
    handle_committed_profile(message);
    return;
  }
  if (message.type == LiveMessageType::begin_edit) {
    handle_begin_edit(message);
    return;
  }

  if (session_ == nullptr || message.edit_session != session_->id) {
    send_reply(LiveMessageType::error, message,
                    "edit session is not active");
    return;
  }
  session_->last_heartbeat = layerc::core::MonotonicClock::now();
  if (layerc::core::revision_decision(message.revision,
                                      session_->revision) ==
      layerc::core::RevisionDecision::stale) {
    send_reply(LiveMessageType::error, message, "stale edit revision");
    return;
  }

  if (message.type == LiveMessageType::heartbeat)
    return;
  if (message.type == LiveMessageType::end_edit) {
    end_session("editor ended session");
    return;
  }
  if (message.type == LiveMessageType::live_profile) {
    const auto parsed = layerc::core::parse_profile_json(message.profile_json);
    const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
    if (profile == nullptr) {
      send_reply(LiveMessageType::error, message,
                      "live profile is invalid");
      return;
    }
    session_->profile = *profile;
    session_->revision = message.revision;
    rebuild_overlays();
    return;
  }
  if (message.type == LiveMessageType::select_panel) {
    session_->revision = message.revision;
    session_->selected_panel = message.panel_id;
    rebuild_overlays();
    return;
  }
  if (message.type == LiveMessageType::set_direct_drag) {
    session_->revision = message.revision;
    session_->direct_drag = message.direct_drag;
    rebuild_overlays();
    return;
  }
  if (message.type == LiveMessageType::anchor_changed) {
    const auto panel =
        std::ranges::find_if(session_->profile.panels,
                             [&](const layerc::core::Panel &candidate) {
                               return candidate.id == message.panel_id;
                             });
    if (panel == session_->profile.panels.end()) {
      send_reply(LiveMessageType::error, message, "unknown panel id");
      return;
    }
    panel->anchor_x = message.anchor_x;
    panel->anchor_y = message.anchor_y;
    session_->revision = message.revision;
    session_->overlays.update_panel(*panel);
    update_overlays();
  }
}

void LiveEditHost::handle_committed_profile(
    const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  const auto parsed = layerc::core::parse_profile_json(message.profile_json);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  const auto &key = message.profile_key;
  if (profile == nullptr || key.empty()) {
    send_reply(LiveMessageType::error, message,
                    "committed profile is invalid");
    return;
  }
  const auto current = std::ranges::find_if(
      sessions_.sources,
      [&](const ProfileSource &source) { return source.key == key; });
  if (current != sessions_.sources.end()) {
    const auto decision = layerc::core::revision_decision(message.revision,
                                                          current->revision);
    const bool duplicate_match =
        decision == layerc::core::RevisionDecision::duplicate &&
        *profile == current->profile;
    if (decision == layerc::core::RevisionDecision::stale ||
        duplicate_match) {
      if (*profile == current->profile) {
        send_reply(LiveMessageType::acknowledgement, message,
                        "profile already applied");
        return;
      }
      send_reply(LiveMessageType::error, message,
                      "stale committed profile revision");
      return;
    }
  }
  control_.apply_profile_update(key, *profile, message.revision);
  if (session_ != nullptr &&
      session_->source_key == key &&
      message.revision >= session_->revision) {
    session_->profile = *profile;
    session_->revision = message.revision;
    rebuild_overlays();
  }
  send_reply(LiveMessageType::acknowledgement, message,
                  "profile applied");
}

void LiveEditHost::handle_begin_edit(const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  const auto parsed = layerc::core::parse_profile_json(message.profile_json);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  const auto &source_key = message.profile_key;
  const auto target = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(message.target_window));
  DWORD target_process_id{};
  if (target != nullptr)
    GetWindowThreadProcessId(target, &target_process_id);
  // Reject own/editor processes and invalid targets before opening a session.
  if (profile == nullptr || message.edit_session == 0 || source_key.empty() ||
      target == nullptr || !IsWindow(target) || IsIconic(target) ||
      target_process_id == 0 || target_process_id == GetCurrentProcessId() ||
      target_process_id == channel_->peer_process_id()) {
    send_reply(LiveMessageType::error, message,
                    "edit target or profile is invalid");
    return;
  }
  begin_session(message, *profile);
  send_reply(LiveMessageType::acknowledgement, message,
                  "edit session started");
}

void LiveEditHost::begin_session(const layerc::core::LiveMessage &message,
                                   layerc::core::GameProfile profile) {
  end_session();
  auto session = std::make_unique<EditSession>();
  session->id = message.edit_session;
  session->revision = message.revision;
  session->source_key = message.profile_key;
  session->profile = std::move(profile);
  session->selected_panel = message.panel_id;
  session->target = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(message.target_window));
  session->last_heartbeat = layerc::core::MonotonicClock::now();
  session_ = std::move(session);
  rebuild_overlays();
  notifications_.sync_foreground_state();
  overlays_.mark_dirty();
}

void LiveEditHost::rebuild_overlays() {
  if (session_ == nullptr)
    return;
  auto &session = *session_;
  session.preview_states.clear();
  session.preview_states.reserve(session.profile.cooldowns.size());
  for (const auto &cooldown : session.profile.cooldowns) {
    session.preview_states.push_back({.id = cooldown.id,
                                      .available_charges = cooldown.max_charges,
                                      .max_charges = cooldown.max_charges,
                                      .next_charge_at = {},
                                      .recovery_deadlines = {},
                                      .active_until = {}});
  }

  const OverlayEditMode edit_mode{
      .direct_drag = session.direct_drag,
      .selected_panel = session.selected_panel,
      .drag_callback = [this](const std::string_view id, const double x,
                              const double y, const bool final) {
        on_direct_anchor(id, x, y, final);
      }};
  session.overlays.rebuild(session.profile, overlays_.capture_exclusion(), &edit_mode);
  update_overlays();
}

void LiveEditHost::update_overlays() {
  if (session_ == nullptr)
    return;
  auto &session = *session_;
  if (session.target == nullptr || !IsWindow(session.target) ||
      !IsWindowVisible(session.target) || IsIconic(session.target)) {
    end_session("target closed or minimized");
    return;
  }
  const auto now = layerc::core::MonotonicClock::now();
  // Edit-mode overlays are always presented visible unless an override says
  // otherwise.
  session.overlays.render_for(session.target, session.preview_states, now);
}

void LiveEditHost::on_direct_anchor(const std::string_view panel_id,
                                 const double anchor_x, const double anchor_y,
                                 const bool final) {
  if (session_ == nullptr)
    return;
  const auto panel = std::ranges::find_if(
      session_->profile.panels, [&](const layerc::core::Panel &candidate) {
        return candidate.id == panel_id;
      });
  if (panel == session_->profile.panels.end())
    return;
  panel->anchor_x = anchor_x;
  panel->anchor_y = anchor_y;
  if (session_->selected_panel != panel_id) {
    session_->selected_panel = std::string(panel_id);
    rebuild_overlays();
  }
  ++session_->outbound_revision;
  (void)channel_->send({
      .type = layerc::core::LiveMessageType::anchor_changed,
      .revision = session_->outbound_revision,
      .edit_session = session_->id,
      .panel_id = std::string(panel_id),
      .anchor_x = anchor_x,
      .anchor_y = anchor_y,
      .final = final,
  });
}

void LiveEditHost::end_session(const std::string_view reason) {
  if (session_ == nullptr)
    return;
  if (!reason.empty() && channel_->connected() &&
      authenticated_) {
    const layerc::core::LiveMessage ended{
        .revision = session_->revision,
        .edit_session = session_->id,
        .profile_key = session_->source_key,
    };
    send_reply(layerc::core::LiveMessageType::end_edit, ended,
                    std::string(reason));
  }
  session_->overlays.hide();
  session_.reset();
  if (stopped_)
    return;
  notifications_.sync_foreground_state();
  overlays_.mark_dirty();
  if (!reason.empty())
    notifications_.update_tooltip(L"LayerD - " + layerc::platform::utf8_to_wide(reason));
}

} // namespace layerc::daemon
