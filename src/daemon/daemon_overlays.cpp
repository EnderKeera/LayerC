#include "daemon_app.hpp"

#include <algorithm>

namespace layerc::daemon {
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
  if (affects_runtime) {
    update_overlays();
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

void DaemonApp::update_overlays() {
  const auto now = layerc::core::MonotonicClock::now();
  bool has_running_cooldown{};

  for (auto &session : sessions_) {
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) || IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(active_input_sessions_.begin(), active_input_sessions_.end(),
                  session.get()) != active_input_sessions_.end();
    if (attached_window_invalid || !input_active) {
      for (auto &overlay : session->overlays) {
        overlay->hide();
      }
      continue;
    }

    session->cooldowns.snapshot(now, session->snapshot_buffer);
    const auto &states = session->snapshot_buffer;
    const bool session_is_running = std::any_of(
        states.begin(), states.end(), [](const layerc::core::CooldownState &state) {
          return state.next_charge_at.has_value();
        });
    has_running_cooldown = has_running_cooldown || session_is_running;

    for (auto &overlay : session->overlays) {
      if (!overlay->should_show(states)) {
        overlay->hide();
        continue;
      }
      if (session->is_global()) {
        overlay->show_on_rect(overlay_target());
      } else {
        // A background game's overlay remains owned by that game and is never
        // raised above the foreground application.
        overlay->show_for(session->game_window);
      }
      overlay->render(states, now);
    }
  }

  if (has_running_cooldown && !overlay_timer_running_) {
    SetTimer(window_, overlay_render_timer, 16, nullptr);
    overlay_timer_running_ = true;
  } else if (!has_running_cooldown && overlay_timer_running_) {
    KillTimer(window_, overlay_render_timer);
    overlay_timer_running_ = false;
  }
}

} // namespace layerc::daemon
