#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

namespace layerc::daemon {

void DaemonApp::send_live_reply(const layerc::core::LiveMessageType type,
                                const layerc::core::LiveMessage &request,
                                std::string text) {
  (void)live_channel_.send({.type = type,
                            .revision = request.revision,
                            .edit_session = request.edit_session,
                            .profile_key = request.profile_key,
                            .text = std::move(text)});
}

void DaemonApp::poll_live_channel() {
  auto results = live_channel_.poll();
  const bool connection_changed = live_channel_.take_connection_changed();
  if (connection_changed) {
    live_package_authenticated_ = false;
    if (!live_channel_.connected()) {
      automation_trace_subscribed_ = false;
      for (auto &session : sessions_) {
        if (!session->automation_runtime) continue;
        session->automation_runtime->set_trace_enabled(false);
        session->automation_runtime->clear_trace();
      }
      end_edit_session("editor disconnected");
    }
  }

  for (auto &result : results) {
    if (const auto *error =
            std::get_if<layerc::core::LiveProtocolError>(&result)) {
      layerc::core::LiveMessage request;
      send_live_reply(layerc::core::LiveMessageType::error, request,
                      error->message);
      continue;
    }
    handle_live_message(std::get<layerc::core::LiveMessage>(result));
  }

  if (edit_session_ != nullptr &&
      layerc::core::MonotonicClock::now() - edit_session_->last_heartbeat >
          std::chrono::seconds(1)) {
    end_edit_session("edit lease expired");
  }
  update_edit_overlays();
  reconcile_runtime_services();
}

void DaemonApp::handle_live_message(
    const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  if (message.type == LiveMessageType::handshake) {
    if (message.text != layerc::core::package_identity("LayerE")) {
      live_package_authenticated_ = false;
      live_channel_.disconnect_peer();
      return;
    }
    live_package_authenticated_ = true;
    send_live_reply(
        LiveMessageType::acknowledgement, message,
        layerc::core::package_identity("LayerD") +
            ";live-profile,mirrored-drag,direct-drag,lease-v1,visual-status-v1,diagnostics-v1,automation-trace-v1");
    (void)live_channel_.send({
        .type = LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(visual_status_),
    });
    return;
  }
  if (!live_package_authenticated_) {
    live_channel_.disconnect_peer();
    return;
  }
  if (message.type == LiveMessageType::diagnostics_request) {
    send_live_reply(LiveMessageType::diagnostics_reply, message,
                    runtime_diagnostics_json());
    return;
  }
  if (message.type == LiveMessageType::retry_visual) {
    for (auto &[window, worker] : isolated_visual_captures_) {
      (void)window;
      worker->retry();
    }
    reconcile_runtime_services();
    send_live_reply(LiveMessageType::acknowledgement, message,
                    "visual runtime retry requested");
    return;
  }
  if (message.type == LiveMessageType::automation_trace_subscribe ||
      message.type == LiveMessageType::automation_trace_unsubscribe) {
    automation_trace_subscribed_ =
        message.type == LiveMessageType::automation_trace_subscribe;
    for (auto &session : sessions_) {
      if (!session->automation_runtime) continue;
      session->automation_runtime->clear_trace();
      session->automation_runtime->set_trace_enabled(
          automation_trace_subscribed_);
    }
    send_live_reply(
        LiveMessageType::acknowledgement, message,
        automation_trace_subscribed_ ? "automation trace subscribed"
                                     : "automation trace unsubscribed");
    return;
  }
  if (message.type == LiveMessageType::retry_automation) {
    bool retried{};
    for (auto &session : sessions_) {
      if (!session->automation_runtime) continue;
      if (!message.profile_key.empty() &&
          session->source_key !=
              layerc::platform::canonical_profile_key(
                  layerc::platform::utf8_to_wide(message.profile_key)))
        continue;
      session->automation_runtime->retry(message.text);
      retried = true;
    }
    send_live_reply(
        retried ? LiveMessageType::acknowledgement
                : LiveMessageType::error,
        message, retried ? "automation retry requested"
                         : "matching automation session was not found");
    return;
  }
  if (message.type == LiveMessageType::committed_profile) {
    const auto parsed = layerc::core::parse_profile_json(message.profile_json);
    const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
    const auto decoded_key =
        layerc::platform::utf8_to_wide(message.profile_key);
    const auto key = decoded_key.empty()
                         ? std::wstring{}
                         : layerc::platform::canonical_profile_key(decoded_key);
    if (profile == nullptr || key.empty()) {
      send_live_reply(LiveMessageType::error, message,
                      "committed profile is invalid");
      return;
    }
    const auto current = std::ranges::find_if(
        profile_sources_, [&](const ProfileSource &source) {
          return source.key == key;
        });
    if (current != profile_sources_.end() &&
        (layerc::core::revision_decision(message.revision,
                                         current->revision) ==
             layerc::core::RevisionDecision::stale ||
         (layerc::core::revision_decision(message.revision,
                                          current->revision) ==
              layerc::core::RevisionDecision::duplicate &&
          *profile == current->profile))) {
      if (*profile == current->profile) {
        send_live_reply(LiveMessageType::acknowledgement, message,
                        "profile already applied");
        return;
      }
      send_live_reply(LiveMessageType::error, message,
                      "stale committed profile revision");
      return;
    }
    apply_profile_update(key, *profile, message.revision);
    if (edit_session_ != nullptr && edit_session_->source_key == key &&
        message.revision >= edit_session_->revision) {
      edit_session_->profile = *profile;
      edit_session_->revision = message.revision;
      rebuild_edit_overlays();
    }
    send_live_reply(LiveMessageType::acknowledgement, message,
                    "profile applied");
    return;
  }
  if (message.type == LiveMessageType::begin_edit) {
    const auto parsed = layerc::core::parse_profile_json(message.profile_json);
    const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
    const auto source_key =
        layerc::platform::utf8_to_wide(message.profile_key);
    const auto target = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(message.target_window));
    DWORD target_process_id{};
    if (target != nullptr)
      GetWindowThreadProcessId(target, &target_process_id);
    if (profile == nullptr || message.edit_session == 0 ||
        source_key.empty() || target == nullptr || !IsWindow(target) ||
        IsIconic(target) || target_process_id == 0 ||
        target_process_id == GetCurrentProcessId() ||
        target_process_id == live_channel_.peer_process_id()) {
      send_live_reply(LiveMessageType::error, message,
                      "edit target or profile is invalid");
      return;
    }
    begin_edit_session(message, *profile);
    send_live_reply(LiveMessageType::acknowledgement, message,
                    "edit session started");
    return;
  }

  if (edit_session_ == nullptr ||
      message.edit_session != edit_session_->id) {
    send_live_reply(LiveMessageType::error, message,
                    "edit session is not active");
    return;
  }
  edit_session_->last_heartbeat = layerc::core::MonotonicClock::now();
  if (layerc::core::revision_decision(message.revision,
                                      edit_session_->revision) ==
      layerc::core::RevisionDecision::stale) {
    send_live_reply(LiveMessageType::error, message,
                    "stale edit revision");
    return;
  }

  if (message.type == LiveMessageType::heartbeat) return;
  if (message.type == LiveMessageType::end_edit) {
    end_edit_session("editor ended session");
    return;
  }
  if (message.type == LiveMessageType::live_profile) {
    const auto parsed = layerc::core::parse_profile_json(message.profile_json);
    const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
    if (profile == nullptr) {
      send_live_reply(LiveMessageType::error, message,
                      "live profile is invalid");
      return;
    }
    edit_session_->profile = *profile;
    edit_session_->revision = message.revision;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::select_panel) {
    edit_session_->revision = message.revision;
    edit_session_->selected_panel = message.panel_id;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::set_direct_drag) {
    edit_session_->revision = message.revision;
    edit_session_->direct_drag = message.direct_drag;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::anchor_changed) {
    const auto panel = std::ranges::find_if(
        edit_session_->profile.panels, [&](const layerc::core::Panel &candidate) {
          return candidate.id == message.panel_id;
        });
    if (panel == edit_session_->profile.panels.end()) {
      send_live_reply(LiveMessageType::error, message, "unknown panel id");
      return;
    }
    panel->anchor_x = message.anchor_x;
    panel->anchor_y = message.anchor_y;
    edit_session_->revision = message.revision;
    edit_session_->overlays.update_panel(*panel);
    update_edit_overlays();
  }
}

void DaemonApp::begin_edit_session(
    const layerc::core::LiveMessage &message,
    layerc::core::GameProfile profile) {
  end_edit_session();
  auto session = std::make_unique<EditSession>();
  session->id = message.edit_session;
  session->revision = message.revision;
  session->source_key = layerc::platform::canonical_profile_key(
      layerc::platform::utf8_to_wide(message.profile_key));
  session->profile = std::move(profile);
  session->selected_panel = message.panel_id;
  session->target = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(message.target_window));
  session->last_heartbeat = layerc::core::MonotonicClock::now();
  edit_session_ = std::move(session);
  rebuild_edit_overlays();
  sync_foreground_state();
  update_overlays();
}

void DaemonApp::rebuild_edit_overlays() {
  if (edit_session_ == nullptr) return;
  auto &session = *edit_session_;
  session.preview_states.clear();
  session.preview_states.reserve(session.profile.cooldowns.size());
  for (const auto &cooldown : session.profile.cooldowns) {
    session.preview_states.push_back({.id = cooldown.id,
                                      .available_charges = cooldown.max_charges,
                                      .max_charges = cooldown.max_charges});
  }

  const OverlayEditMode edit_mode{
      .direct_drag = session.direct_drag,
      .selected_panel = session.selected_panel,
      .drag_callback =
          [this](const std::string_view id, const double x, const double y,
                 const bool final) { on_direct_anchor(id, x, y, final); }};
  session.overlays.rebuild(session.profile, capture_exclusion_, &edit_mode);
  update_edit_overlays();
}

void DaemonApp::update_edit_overlays() {
  if (edit_session_ == nullptr) return;
  if (edit_session_->target == nullptr || !IsWindow(edit_session_->target) ||
      !IsWindowVisible(edit_session_->target) || IsIconic(edit_session_->target)) {
    end_edit_session("target closed or minimized");
    return;
  }
  const auto now = layerc::core::MonotonicClock::now();
  edit_session_->overlays.render_for(edit_session_->target,
                                     edit_session_->preview_states, now, true);
}

void DaemonApp::on_direct_anchor(const std::string_view panel_id,
                                 const double anchor_x,
                                 const double anchor_y, const bool final) {
  if (edit_session_ == nullptr) return;
  const auto panel = std::ranges::find_if(
      edit_session_->profile.panels, [&](const layerc::core::Panel &candidate) {
        return candidate.id == panel_id;
      });
  if (panel == edit_session_->profile.panels.end()) return;
  panel->anchor_x = anchor_x;
  panel->anchor_y = anchor_y;
  if (edit_session_->selected_panel != panel_id) {
    edit_session_->selected_panel = std::string(panel_id);
    rebuild_edit_overlays();
  }
  ++edit_session_->outbound_revision;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::anchor_changed,
      .revision = edit_session_->outbound_revision,
      .edit_session = edit_session_->id,
      .panel_id = std::string(panel_id),
      .anchor_x = anchor_x,
      .anchor_y = anchor_y,
      .final = final,
  });
}

void DaemonApp::end_edit_session(const std::string_view reason) {
  if (edit_session_ == nullptr) return;
  if (!reason.empty() && live_channel_.connected() &&
      live_package_authenticated_) {
    const layerc::core::LiveMessage ended{
        .revision = edit_session_->revision,
        .edit_session = edit_session_->id,
        .profile_key = layerc::platform::wide_to_utf8(
            edit_session_->source_key),
    };
    send_live_reply(layerc::core::LiveMessageType::end_edit, ended,
                    std::string(reason));
  }
  edit_session_->overlays.hide();
  edit_session_.reset();
  if (shutting_down_) return;
  sync_foreground_state();
  update_overlays();
  if (!reason.empty())
    update_tooltip(L"LayerD - " + layerc::platform::utf8_to_wide(reason));
}

} // namespace layerc::daemon
