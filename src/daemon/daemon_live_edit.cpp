#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <utility>
#include <variant>

namespace layerc::daemon {
namespace {

std::vector<OverlayCooldown> edit_overlay_cooldowns(
    const std::vector<layerc::core::CooldownDefinition> &definitions) {
  std::vector<OverlayCooldown> result;
  result.reserve(definitions.size());
  for (const auto &definition : definitions) {
    result.push_back({.id = definition.id,
                      .name = layerc::platform::utf8_to_wide(definition.name),
                      .duration = definition.duration});
  }
  return result;
}

} // namespace

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
  const bool connection_changed = live_channel_.take_connection_changed();
  if (connection_changed && !live_channel_.connected())
    end_edit_session("editor disconnected");

  for (auto &result : live_channel_.poll()) {
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
  const UINT desired_interval = live_channel_.connected() ? 16U : 250U;
  if (desired_interval != live_timer_interval_) {
    live_timer_interval_ = desired_interval;
    SetTimer(window_, live_channel_timer, live_timer_interval_, nullptr);
  }
}

void DaemonApp::handle_live_message(
    const layerc::core::LiveMessage &message) {
  using layerc::core::LiveMessageType;
  if (message.type == LiveMessageType::handshake) {
    send_live_reply(LiveMessageType::acknowledgement, message,
                    "live-profile,mirrored-drag,direct-drag,lease-v1");
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
    const auto overlay = std::ranges::find_if(
        edit_session_->overlays, [&](const auto &candidate) {
          return candidate->panel_id() == panel->id;
        });
    if (overlay != edit_session_->overlays.end())
      (*overlay)->update_snapshot(*panel, edit_session_->overlay_cooldowns);
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
  session.overlay_cooldowns = edit_overlay_cooldowns(session.profile.cooldowns);
  session.preview_states.clear();
  session.preview_states.reserve(session.profile.cooldowns.size());
  for (const auto &cooldown : session.profile.cooldowns) {
    session.preview_states.push_back({.id = cooldown.id,
                                      .available_charges = cooldown.max_charges,
                                      .max_charges = cooldown.max_charges});
  }

  std::vector<std::unique_ptr<OverlayWindow>> next;
  next.reserve(session.profile.panels.size());
  for (const auto &panel : session.profile.panels) {
    auto found = std::ranges::find_if(
        session.overlays, [&](const auto &candidate) {
          return candidate != nullptr && candidate->panel_id() == panel.id;
        });
    std::unique_ptr<OverlayWindow> overlay;
    if (found != session.overlays.end()) {
      overlay = std::move(*found);
      overlay->update_snapshot(panel, session.overlay_cooldowns);
    } else {
      overlay = std::make_unique<OverlayWindow>(panel,
                                                session.overlay_cooldowns);
      overlay->set_capture_exclusion(capture_exclusion_);
    }
    overlay->set_edit_mode(
        true, session.direct_drag, panel.id == session.selected_panel,
        [this](const std::string_view id, const double x, const double y,
               const bool final) { on_direct_anchor(id, x, y, final); });
    next.push_back(std::move(overlay));
  }
  session.overlays = std::move(next);
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
  for (auto &overlay : edit_session_->overlays) {
    overlay->show_for(edit_session_->target);
    overlay->render(edit_session_->preview_states, now);
  }
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
  if (!reason.empty() && live_channel_.connected()) {
    const layerc::core::LiveMessage ended{
        .revision = edit_session_->revision,
        .edit_session = edit_session_->id,
        .profile_key = layerc::platform::wide_to_utf8(
            edit_session_->source_key),
    };
    send_live_reply(layerc::core::LiveMessageType::end_edit, ended,
                    std::string(reason));
  }
  for (auto &overlay : edit_session_->overlays) overlay->hide();
  edit_session_.reset();
  sync_foreground_state();
  update_overlays();
  if (!reason.empty())
    update_tooltip(L"LayerD - " + layerc::platform::utf8_to_wide(reason));
}

} // namespace layerc::daemon
