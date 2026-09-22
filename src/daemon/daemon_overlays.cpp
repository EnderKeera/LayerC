#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace layerc::daemon {
namespace {

void apply_property(layerc::core::Panel &panel, std::optional<bool> &visible,
                    std::optional<std::wstring> &text,
                    const layerc::core::OverlayPropertyOverride &item) {
  const auto *spec = layerc::core::find_overlay_property(item.property);
  if (spec == nullptr)
    return;
  if (spec->panel_visibility) {
    if (const auto *boolean = std::get_if<bool>(&item.value.storage))
      visible = *boolean;
  } else if (spec->panel_text) {
    if (const auto *string = std::get_if<std::string>(&item.value.storage))
      text = layerc::platform::utf8_to_wide(*string);
  } else if (spec->apply != nullptr) {
    spec->apply(panel, item.value);
  }
}

void apply_automation_overrides(Session &session) {
  static const std::vector<layerc::core::OverlayPropertyOverride> empty;
  const auto &overrides =
      session.automation_runtime
          ? session.automation_runtime->snapshot().overlay_overrides
          : empty;
  if (overrides == session.applied_overlay_overrides)
    return;
  session.applied_overlay_overrides = overrides;

  for (const auto &base_panel : session.profile.panels) {
    auto panel = base_panel;
    std::optional<bool> visible;
    std::optional<std::wstring> text;
    std::map<std::string, const layerc::core::OverlayPropertyOverride *,
             std::less<>>
        winners;
    for (const auto &item : overrides) {
      if (item.panel_id != panel.id)
        continue;
      const auto found = winners.find(item.property);
      if (found == winners.end() ||
          layerc::core::overlay_override_precedes(item, *found->second))
        winners[item.property] = &item;
    }
    for (const auto &[property, item] : winners) {
      (void)property;
      apply_property(panel, visible, text, *item);
    }
    session.overlays.apply_session_override(panel, visible, std::move(text));
  }
}

} // namespace

void DaemonApp::on_overlay_event(const DWORD event, const HWND window) {
  if (event == EVENT_SYSTEM_FOREGROUND)
    sync_foreground_state();
  else if (event == EVENT_OBJECT_DESTROY)
    refresh_game_windows();
  else if (event == EVENT_OBJECT_LOCATIONCHANGE)
    sync_overlay_location(window);
}

void DaemonApp::sync_overlay_location(const HWND changed_window) {
  const HWND changed_root = GetAncestor(changed_window, GA_ROOT);
  if (changed_root == nullptr) {
    return;
  }
  if (IsIconic(changed_root) || !IsWindowVisible(changed_root)) {
    refresh_game_windows();
    return;
  }
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const bool affects_runtime = std::any_of(
      session_registry_.sessions().begin(), session_registry_.sessions().end(),
      [&](const std::unique_ptr<Session> &session) {
        return (!session->is_global() &&
                session->game_window == changed_root) ||
               (session->is_global() && changed_root == foreground_root);
      });
  const bool affects_edit =
      live_edit_host_.session() != nullptr && live_edit_host_.session()->target == changed_root;
  if (affects_runtime || affects_edit) {
    update_overlays();
    update_edit_overlays();
  }
}

RECT DaemonApp::overlay_target() const {
  RECT target{};
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  if (foreground != nullptr && foreground != window_ && !IsIconic(foreground)) {
    RECT client{};
    POINT origin{};
    if (GetClientRect(foreground, &client) &&
        ClientToScreen(foreground, &origin)) {
      target = {origin.x, origin.y, origin.x + client.right - client.left,
                origin.y + client.bottom - client.top};
      if (target.right > target.left && target.bottom > target.top) {
        return target;
      }
    }
  }
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0) &&
      target.right > target.left && target.bottom > target.top) {
    return target;
  }
  return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void DaemonApp::reconcile_overlay_runtime() {
  for (auto *session : session_registry_.active_inputs()) {
    if (session != nullptr)
      session->overlays.ensure(session->profile, capture_exclusion_);
  }
  update_overlays();
}

void DaemonApp::quiesce_overlay_runtime() noexcept {
  if (overlay_runtime_.timer_running() && window_ != nullptr)
    KillTimer(window_, overlay_render_timer);
  overlay_runtime_.set_timer_running(false);
  for (auto &session : session_registry_.sessions())
    session->overlays.release();
  if (live_edit_host_.session() != nullptr) {
    live_edit_host_.session()->overlays.release();
  }
}

void DaemonApp::update_overlays() {
  const auto now = layerc::core::MonotonicClock::now();
  publish_companion_snapshots(now);
  bool has_running_cooldown{};

  for (auto &session : session_registry_.sessions()) {
    apply_automation_overrides(*session);
    const bool hidden_for_edit = live_edit_host_.session() != nullptr &&
                                 !session->is_global() &&
                                 session->game_window == live_edit_host_.session()->target;
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) ||
         IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(session_registry_.active_inputs().begin(), session_registry_.active_inputs().end(),
                  session.get()) != session_registry_.active_inputs().end();
    if (hidden_for_edit || attached_window_invalid || !input_active) {
      session->overlays.hide();
      continue;
    }

    session->cooldowns.snapshot(now, session->snapshot_buffer);
    const auto &states = session->snapshot_buffer;
    const bool session_is_running =
        std::any_of(states.begin(), states.end(),
                    [](const layerc::core::CooldownState &state) {
                      return state.next_charge_at.has_value();
                    });
    has_running_cooldown = has_running_cooldown || session_is_running;

    if (session->is_global())
      session->overlays.render_on_rect(overlay_target(), states, now);
    else
      session->overlays.render_for(session->game_window, states, now);
  }

  sync_visual_exclusions();

  if (has_running_cooldown && !overlay_runtime_.timer_running()) {
    SetTimer(window_, overlay_render_timer, 16, nullptr);
    overlay_runtime_.set_timer_running(true);
  } else if (!has_running_cooldown && overlay_runtime_.timer_running()) {
    KillTimer(window_, overlay_render_timer);
    overlay_runtime_.set_timer_running(false);
  }
}

void DaemonApp::publish_companion_snapshots(const layerc::core::TimePoint now) {
  if (companion_host_.transport().wait_handle() == nullptr) {
    companion_host_.bridge().clear_snapshot();
    return;
  }
  const auto foreground = current_companion_sessions();
  if (foreground.empty()) {
    companion_host_.bridge().clear_snapshot();
  } else {
    auto *session = foreground.front();
    if (session != nullptr) {
      session->cooldowns.snapshot(now, session->snapshot_buffer);
      const auto session_key =
          layerc::platform::window_session_key(session->game_window);
      const auto snapshot =
          layerc::gamebar::GameBarBridge::create_session_snapshot(
              session->profile.id, session_key, session->profile.panels,
              session->profile.cooldowns, session->snapshot_buffer,
              session->modules.companion.profile, now);
      companion_host_.bridge().update_snapshot(snapshot);
      if (const auto json = companion_host_.bridge().latest_snapshot_json(now);
          json.has_value())
        (void)companion_host_.transport().publish(*json);
    } else {
      companion_host_.bridge().clear_snapshot();
    }
  }
}

void DaemonApp::poll_gamebar_transport() {
  bool reload_needed{};
  for (auto &update : companion_host_.transport().poll()) {
    const bool accepted = apply_companion_preferences(update.json);
    companion_host_.transport().complete_preference_update(update.request_id, accepted);
    reload_needed = reload_needed || accepted;
  }
  if (reload_needed)
    reload_profiles();
}

bool DaemonApp::apply_companion_preferences(const std::string_view json) {
  for (const auto &source : session_registry_.sources()) {
    const auto parsed =
        layerc::core::parse_companion_profile_json(json, source.profile);
    const auto *profile = std::get_if<layerc::core::CompanionProfile>(&parsed);
    if (profile == nullptr)
      continue;
    if (!layerc::platform::may_write_primary(source.modules.visual.state) ||
        !layerc::platform::may_write_primary(source.modules.automation.state))
      return false;
    if (!layerc::platform::write_profile_bundle(
            source.path, source.profile, source.modules.visual.profile,
            source.modules.automation.profile, *profile))
      return false;
    return true;
  }
  return false;
}

} // namespace layerc::daemon
