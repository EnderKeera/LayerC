#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

namespace layerc::daemon {

void DaemonApp::send_live_reply(const layerc::core::LiveMessageType type,
                                const layerc::core::LiveMessage &request,
                                std::string text) {
  (void)live_edit_host_.channel().send({.type = type,
                            .revision = request.revision,
                            .edit_session = request.edit_session,
                            .profile_key = request.profile_key,
                            .text = std::move(text)});
}

void DaemonApp::poll_live_channel() {
  auto results = live_edit_host_.channel().poll();
  const bool connection_changed = live_edit_host_.channel().take_connection_changed();
  if (connection_changed) {
    live_edit_host_.set_authenticated(false);
    if (!live_edit_host_.channel().connected()) {
      automation_runtime_host_.set_trace_subscribed(false);
      for (auto &session : session_registry_.sessions()) {
        if (!session->automation_runtime)
          continue;
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

  if (live_edit_host_.session() != nullptr &&
      layerc::core::MonotonicClock::now() - live_edit_host_.session()->last_heartbeat >
          std::chrono::seconds(1)) {
    end_edit_session("edit lease expired");
  }
  update_edit_overlays();
  reconcile_runtime_services();
}

void DaemonApp::handle_live_message(const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  if (message.type == LiveMessageType::handshake) {
    if (message.text != layerc::core::package_identity("LayerE")) {
      live_edit_host_.set_authenticated(false);
      live_edit_host_.channel().disconnect_peer();
      return;
    }
    live_edit_host_.set_authenticated(true);
    send_live_reply(LiveMessageType::acknowledgement, message,
                    layerc::core::package_identity("LayerD") +
                        ";live-profile,mirrored-drag,direct-drag,lease-v1,"
                        "visual-status-v1,diagnostics-v1,automation-trace-v1");
    (void)live_edit_host_.channel().send({
        .type = LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(visual_capture_runtime_.status()),
    });
    return;
  }
  if (!live_edit_host_.authenticated()) {
    live_edit_host_.channel().disconnect_peer();
    return;
  }
  if (message.type == LiveMessageType::diagnostics_request) {
    send_live_reply(LiveMessageType::diagnostics_reply, message,
                    runtime_diagnostics_json());
    return;
  }
  if (message.type == LiveMessageType::retry_visual) {
    for (auto &[window, worker] : visual_capture_runtime_.captures()) {
      (void)window;
      if (worker)
        worker->retry();
    }
    reconcile_runtime_services();
    send_live_reply(LiveMessageType::acknowledgement, message,
                    "visual runtime retry requested");
    return;
  }
  if (message.type == LiveMessageType::automation_trace_subscribe ||
      message.type == LiveMessageType::automation_trace_unsubscribe) {
    automation_runtime_host_.set_trace_subscribed(
        message.type == LiveMessageType::automation_trace_subscribe);
    for (auto &session : session_registry_.sessions()) {
      if (!session->automation_runtime)
        continue;
      session->automation_runtime->clear_trace();
      session->automation_runtime->set_trace_enabled(
          automation_runtime_host_.trace_subscribed());
    }
    send_live_reply(LiveMessageType::acknowledgement, message,
                    automation_runtime_host_.trace_subscribed()
                        ? "automation trace subscribed"
                        : "automation trace unsubscribed");
    return;
  }
  if (message.type == LiveMessageType::retry_automation) {
    bool retried{};
    for (auto &session : session_registry_.sessions()) {
      if (!session->automation_runtime)
        continue;
      if (!message.profile_key.empty() &&
          session->source_key != message.profile_key)
        continue;
      session->automation_runtime->retry(message.text);
      retried = true;
    }
    send_live_reply(retried ? LiveMessageType::acknowledgement
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

  if (live_edit_host_.session() == nullptr || message.edit_session != live_edit_host_.session()->id) {
    send_live_reply(LiveMessageType::error, message,
                    "edit session is not active");
    return;
  }
  live_edit_host_.session()->last_heartbeat = layerc::core::MonotonicClock::now();
  if (layerc::core::revision_decision(message.revision,
                                      live_edit_host_.session()->revision) ==
      layerc::core::RevisionDecision::stale) {
    send_live_reply(LiveMessageType::error, message, "stale edit revision");
    return;
  }

  if (message.type == LiveMessageType::heartbeat)
    return;
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
    live_edit_host_.session()->profile = *profile;
    live_edit_host_.session()->revision = message.revision;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::select_panel) {
    live_edit_host_.session()->revision = message.revision;
    live_edit_host_.session()->selected_panel = message.panel_id;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::set_direct_drag) {
    live_edit_host_.session()->revision = message.revision;
    live_edit_host_.session()->direct_drag = message.direct_drag;
    rebuild_edit_overlays();
    return;
  }
  if (message.type == LiveMessageType::anchor_changed) {
    const auto panel =
        std::ranges::find_if(live_edit_host_.session()->profile.panels,
                             [&](const layerc::core::Panel &candidate) {
                               return candidate.id == message.panel_id;
                             });
    if (panel == live_edit_host_.session()->profile.panels.end()) {
      send_live_reply(LiveMessageType::error, message, "unknown panel id");
      return;
    }
    panel->anchor_x = message.anchor_x;
    panel->anchor_y = message.anchor_y;
    live_edit_host_.session()->revision = message.revision;
    live_edit_host_.session()->overlays.update_panel(*panel);
    update_edit_overlays();
  }
}

void DaemonApp::handle_committed_profile(
    const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  const auto parsed = layerc::core::parse_profile_json(message.profile_json);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  const auto &key = message.profile_key;
  if (profile == nullptr || key.empty()) {
    send_live_reply(LiveMessageType::error, message,
                    "committed profile is invalid");
    return;
  }
  const auto current = std::ranges::find_if(
      session_registry_.sources(),
      [&](const ProfileSource &source) { return source.key == key; });
  if (current != session_registry_.sources().end()) {
    const auto decision = layerc::core::revision_decision(message.revision,
                                                          current->revision);
    const bool duplicate_match =
        decision == layerc::core::RevisionDecision::duplicate &&
        *profile == current->profile;
    if (decision == layerc::core::RevisionDecision::stale ||
        duplicate_match) {
      if (*profile == current->profile) {
        send_live_reply(LiveMessageType::acknowledgement, message,
                        "profile already applied");
        return;
      }
      send_live_reply(LiveMessageType::error, message,
                      "stale committed profile revision");
      return;
    }
  }
  apply_profile_update(key, *profile, message.revision);
  if (live_edit_host_.session() != nullptr &&
      live_edit_host_.session()->source_key == key &&
      message.revision >= live_edit_host_.session()->revision) {
    live_edit_host_.session()->profile = *profile;
    live_edit_host_.session()->revision = message.revision;
    rebuild_edit_overlays();
  }
  send_live_reply(LiveMessageType::acknowledgement, message,
                  "profile applied");
}

void DaemonApp::handle_begin_edit(const layerc::core::LiveMessage &message) {
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
      target_process_id == live_edit_host_.channel().peer_process_id()) {
    send_live_reply(LiveMessageType::error, message,
                    "edit target or profile is invalid");
    return;
  }
  begin_edit_session(message, *profile);
  send_live_reply(LiveMessageType::acknowledgement, message,
                  "edit session started");
}

void DaemonApp::begin_edit_session(const layerc::core::LiveMessage &message,
                                   layerc::core::GameProfile profile) {
  end_edit_session();
  auto session = std::make_unique<EditSession>();
  session->id = message.edit_session;
  session->revision = message.revision;
  session->source_key = message.profile_key;
  session->profile = std::move(profile);
  session->selected_panel = message.panel_id;
  session->target = reinterpret_cast<HWND>(
      static_cast<std::uintptr_t>(message.target_window));
  session->last_heartbeat = layerc::core::MonotonicClock::now();
  live_edit_host_.session() = std::move(session);
  rebuild_edit_overlays();
  sync_foreground_state();
  mark_overlays_dirty();
}

void DaemonApp::rebuild_edit_overlays() {
  if (live_edit_host_.session() == nullptr)
    return;
  auto &session = *live_edit_host_.session();
  session.preview_states.clear();
  session.preview_states.reserve(session.profile.cooldowns.size());
  for (const auto &cooldown : session.profile.cooldowns) {
    session.preview_states.push_back({.id = cooldown.id,
                                      .available_charges = cooldown.max_charges,
                                      .max_charges = cooldown.max_charges,
                                      .next_charge_at = {},
                                      .recovery_deadlines = {}});
  }

  const OverlayEditMode edit_mode{
      .direct_drag = session.direct_drag,
      .selected_panel = session.selected_panel,
      .drag_callback = [this](const std::string_view id, const double x,
                              const double y, const bool final) {
        on_direct_anchor(id, x, y, final);
      }};
  session.overlays.rebuild(session.profile, capture_exclusion_, &edit_mode);
  update_edit_overlays();
}

void DaemonApp::update_edit_overlays() {
  if (live_edit_host_.session() == nullptr)
    return;
  auto &session = *live_edit_host_.session();
  if (session.target == nullptr || !IsWindow(session.target) ||
      !IsWindowVisible(session.target) || IsIconic(session.target)) {
    end_edit_session("target closed or minimized");
    return;
  }
  const auto now = layerc::core::MonotonicClock::now();
  session.overlays.render_for(session.target, session.preview_states, now,
                              true);
}

void DaemonApp::on_direct_anchor(const std::string_view panel_id,
                                 const double anchor_x, const double anchor_y,
                                 const bool final) {
  if (live_edit_host_.session() == nullptr)
    return;
  const auto panel = std::ranges::find_if(
      live_edit_host_.session()->profile.panels, [&](const layerc::core::Panel &candidate) {
        return candidate.id == panel_id;
      });
  if (panel == live_edit_host_.session()->profile.panels.end())
    return;
  panel->anchor_x = anchor_x;
  panel->anchor_y = anchor_y;
  if (live_edit_host_.session()->selected_panel != panel_id) {
    live_edit_host_.session()->selected_panel = std::string(panel_id);
    rebuild_edit_overlays();
  }
  ++live_edit_host_.session()->outbound_revision;
  (void)live_edit_host_.channel().send({
      .type = layerc::core::LiveMessageType::anchor_changed,
      .revision = live_edit_host_.session()->outbound_revision,
      .edit_session = live_edit_host_.session()->id,
      .panel_id = std::string(panel_id),
      .anchor_x = anchor_x,
      .anchor_y = anchor_y,
      .final = final,
  });
}

void DaemonApp::end_edit_session(const std::string_view reason) {
  if (live_edit_host_.session() == nullptr)
    return;
  if (!reason.empty() && live_edit_host_.channel().connected() &&
      live_edit_host_.authenticated()) {
    const layerc::core::LiveMessage ended{
        .revision = live_edit_host_.session()->revision,
        .edit_session = live_edit_host_.session()->id,
        .profile_key = live_edit_host_.session()->source_key,
    };
    send_live_reply(layerc::core::LiveMessageType::end_edit, ended,
                    std::string(reason));
  }
  live_edit_host_.session()->overlays.hide();
  live_edit_host_.session().reset();
  if (shutting_down_)
    return;
  sync_foreground_state();
  mark_overlays_dirty();
  if (!reason.empty())
    update_tooltip(L"LayerD - " + layerc::platform::utf8_to_wide(reason));
}

} // namespace layerc::daemon
