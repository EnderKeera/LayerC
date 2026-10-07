#include "daemon_state.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace layerc::daemon {
namespace {

// No "already applied" cache: a released and recreated collection, or a
// rebuilt base panel, must receive the still-active overrides again.
void apply_automation_overrides(Session &session) {
  if (session.automation.runtime())
    session.overlays.apply_overrides(
        session.automation.profile(), session.automation.runtime()->snapshot().overlay_overrides);
  else
    session.overlays.apply_overrides(session.automation.profile(), {});
}

} // namespace

void OverlayRuntime::sync_location(const HWND changed_window) {
  const HWND changed_root = GetAncestor(changed_window, GA_ROOT);
  if (changed_root == nullptr) {
    return;
  }
  if (IsIconic(changed_root) || !IsWindowVisible(changed_root)) {
    control_.refresh_game_windows();
    return;
  }
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const bool affects_runtime = std::any_of(
      sessions_.sessions.begin(), sessions_.sessions.end(),
      [&](const std::unique_ptr<Session> &session) {
        return (!session->is_global() &&
                session->game_window == changed_root) ||
               (session->is_global() && changed_root == foreground_root);
      });
  const bool affects_edit =
      live_edit_.edit_session() != nullptr && live_edit_.edit_session()->target == changed_root;
  if (affects_runtime || affects_edit) {
    mark_dirty();
    live_edit_.update_overlays();
  }
}

RECT OverlayRuntime::target_rect() const {
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

layerc::runtime::ServiceReconcileResult
OverlayRuntime::reconcile(const layerc::runtime::CapabilityDemand &) {
  active_ = true;
  for (auto *session : sessions_.active_inputs) {
    if (session != nullptr)
      session->overlays.ensure(session->automation.profile(), capture_exclusion_);
  }
  mark_dirty();
  return {};
}

void OverlayRuntime::quiesce() noexcept {
  active_ = false;
  if (timer_running_ && window_ != nullptr)
    KillTimer(window_, overlay_render_timer);
  timer_running_ = false;
  for (auto &session : sessions_.sessions)
    session->overlays.release();

}

void OverlayRuntime::flush() {
  if (!std::exchange(dirty_, false))
    return;
  const auto now = layerc::core::MonotonicClock::now();
  companion_.publish_snapshot(now);
  // A quiesced renderer never recreates its timer or native resources.
  if (active_)
    render(now);
}

void OverlayRuntime::render(const layerc::core::TimePoint now) {
  bool has_running_cooldown{};

  for (auto &session : sessions_.sessions) {
    apply_automation_overrides(*session);
    const bool hidden_for_edit = live_edit_.edit_session() != nullptr &&
                                 !session->is_global() &&
                                 session->game_window == live_edit_.edit_session()->target;
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) ||
         IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(sessions_.active_inputs.begin(), sessions_.active_inputs.end(),
                  session.get()) != sessions_.active_inputs.end();
    if (hidden_for_edit || attached_window_invalid || !input_active) {
      session->overlays.hide();
      continue;
    }

    (void)session->automation.snapshot(now);
    const auto &states = session->automation.snapshot_buffer();
    const bool session_is_running =
        std::any_of(states.begin(), states.end(),
                    [](const layerc::core::CooldownState &state) {
                      return !layerc::core::cooldown_ready(state);
                    });
    has_running_cooldown = has_running_cooldown || session_is_running;

    if (session->is_global())
      session->overlays.render_on_rect(target_rect(), states, now);
    else
      session->overlays.render_for(session->game_window, states, now);
  }

  captures_.sync_exclusions();

  if (has_running_cooldown && !timer_running_) {
    timer_running_ =
        window_ != nullptr &&
        SetTimer(window_, overlay_render_timer, 16, nullptr) != 0;
  } else if (!has_running_cooldown && timer_running_) {
    KillTimer(window_, overlay_render_timer);
    timer_running_ = false;
  }
}

} // namespace layerc::daemon
