#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace layerc::daemon {
namespace {

bool override_precedes(const layerc::core::OverlayPropertyOverride &left,
                       const layerc::core::OverlayPropertyOverride &right) {
  if (left.priority != right.priority) return left.priority > right.priority;
  if (left.automation_order != right.automation_order)
    return left.automation_order < right.automation_order;
  return left.rule_order < right.rule_order;
}

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
  if (overrides == session.applied_overlay_overrides) return;
  session.applied_overlay_overrides = overrides;

  for (const auto &base_panel : session.profile.panels) {
    auto panel = base_panel;
    std::optional<bool> visible;
    std::optional<std::wstring> text;
    std::map<std::string,
             const layerc::core::OverlayPropertyOverride *,
             std::less<>>
        winners;
    for (const auto &item : overrides) {
      if (item.panel_id != panel.id) continue;
      const auto found = winners.find(item.property);
      if (found == winners.end() ||
          override_precedes(item, *found->second))
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

void CALLBACK DaemonApp::foreground_event_proc(
    const HWINEVENTHOOK hook, const DWORD event, const HWND window,
    const LONG object_id, const LONG child_id, const DWORD event_thread,
    const DWORD event_time) {
  (void)hook;
  (void)child_id;
  (void)event_thread;
  (void)event_time;
  if (foreground_hook_target_ == nullptr) {
    return;
  }
  if (event == EVENT_SYSTEM_FOREGROUND) {
    foreground_hook_target_->sync_foreground_state();
  } else if (event == EVENT_OBJECT_DESTROY && object_id == OBJID_WINDOW) {
    foreground_hook_target_->refresh_game_windows();
  } else if (event == EVENT_OBJECT_LOCATIONCHANGE && object_id == OBJID_WINDOW) {
    foreground_hook_target_->sync_overlay_location(window);
  }
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
      sessions_.begin(), sessions_.end(), [&](const std::unique_ptr<Session> &session) {
        return (!session->is_global() && session->game_window == changed_root) ||
               (session->is_global() && changed_root == foreground_root);
      });
  const bool affects_edit = edit_session_ != nullptr &&
                            edit_session_->target == changed_root;
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
    if (GetClientRect(foreground, &client) && ClientToScreen(foreground, &origin)) {
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
  for (auto *session : active_input_sessions_) {
    if (session != nullptr) session->ensure_overlays(capture_exclusion_);
  }
  update_overlays();
}

void DaemonApp::quiesce_overlay_runtime() noexcept {
  if (overlay_timer_running_ && window_ != nullptr)
    KillTimer(window_, overlay_render_timer);
  overlay_timer_running_ = false;
  for (auto &session : sessions_) session->release_overlays();
  if (edit_session_ != nullptr) {
    edit_session_->overlays.release();
  }
}

void DaemonApp::update_overlays() {
  const auto now = layerc::core::MonotonicClock::now();
  publish_companion_snapshots(now);
  bool has_running_cooldown{};

  for (auto &session : sessions_) {
    apply_automation_overrides(*session);
    const bool hidden_for_edit =
        edit_session_ != nullptr && !session->is_global() &&
        session->game_window == edit_session_->target;
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) || IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(active_input_sessions_.begin(), active_input_sessions_.end(),
                  session.get()) != active_input_sessions_.end();
    if (hidden_for_edit || attached_window_invalid || !input_active) {
      session->overlays.hide();
      continue;
    }

    session->cooldowns.snapshot(now, session->snapshot_buffer);
    const auto &states = session->snapshot_buffer;
    const bool session_is_running = std::any_of(
        states.begin(), states.end(), [](const layerc::core::CooldownState &state) {
          return state.next_charge_at.has_value();
        });
    has_running_cooldown = has_running_cooldown || session_is_running;

    if (session->is_global())
      session->overlays.render_on_rect(overlay_target(), states, now);
    else
      session->overlays.render_for(session->game_window, states, now);
  }

  if (has_running_cooldown && !overlay_timer_running_) {
    SetTimer(window_, overlay_render_timer, 16, nullptr);
    overlay_timer_running_ = true;
  } else if (!has_running_cooldown && overlay_timer_running_) {
    KillTimer(window_, overlay_render_timer);
    overlay_timer_running_ = false;
  }
}

void DaemonApp::publish_companion_snapshots(
    const layerc::core::TimePoint now) {
  const auto foreground = foreground_sessions();
  if (foreground.empty()) {
    gamebar_bridge_.clear_snapshot();
    gamebar_transport_.poll();
    return;
  }
  for (auto *session : foreground) {
    if (session == nullptr || !session->has_companion_slots()) continue;
    session->cooldowns.snapshot(now, session->snapshot_buffer);
    const auto snapshot = layerc::gamebar::GameBarBridge::create_session_snapshot(
        session->profile.id, layerc::platform::wide_to_utf8(session->source_key),
        session->profile.panels, session->profile.cooldowns,
        session->snapshot_buffer, session->companion_profile, now);
    gamebar_bridge_.update_snapshot(snapshot);
    if (const auto json = gamebar_bridge_.latest_snapshot_json(now);
        json.has_value())
      (void)gamebar_transport_.publish(*json);
    gamebar_transport_.poll();
    return;
  }
  gamebar_bridge_.clear_snapshot();
  gamebar_transport_.poll();
}

} // namespace layerc::daemon
