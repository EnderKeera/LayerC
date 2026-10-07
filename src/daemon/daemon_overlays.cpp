#include "daemon_app.hpp"

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

void DaemonApp::on_overlay_event(const DWORD event, const HWND window) {
  if (event == EVENT_SYSTEM_FOREGROUND)
    sync_foreground_state();
  else if (event == EVENT_OBJECT_DESTROY)
    refresh_game_windows();
  else if (event == EVENT_OBJECT_LOCATIONCHANGE)
    sync_overlay_location(window);
  // WinEvents also arrive inside modal loops (tray menu), which bypass the
  // run-loop flush.
  flush_overlays();
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
      session_registry_.sessions.begin(), session_registry_.sessions.end(),
      [&](const std::unique_ptr<Session> &session) {
        return (!session->is_global() &&
                session->game_window == changed_root) ||
               (session->is_global() && changed_root == foreground_root);
      });
  const bool affects_edit =
      live_edit_host_.session != nullptr && live_edit_host_.session->target == changed_root;
  if (affects_runtime || affects_edit) {
    overlay_runtime_.mark_dirty();
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

layerc::runtime::ServiceReconcileResult
OverlayRuntime::reconcile(const layerc::runtime::CapabilityDemand &) {
  for (auto *session : sessions_.active_inputs) {
    if (session != nullptr)
      session->overlays.ensure(session->automation.profile(), capture_exclusion);
  }
  mark_dirty();
  return {};
}

void OverlayRuntime::quiesce() noexcept {
  if (timer_running && window_ != nullptr)
    KillTimer(window_, overlay_render_timer);
  timer_running = false;
  for (auto &session : sessions_.sessions)
    session->overlays.release();
  if (live_edit_.session != nullptr)
    live_edit_.session->overlays.release();
}

void DaemonApp::flush_overlays() {
  if (!overlay_runtime_.take_dirty())
    return;
  // WM_DESTROY quiesces the runtime; never render after that.
  if (shutting_down_)
    return;
  render_overlays();
}

void DaemonApp::render_overlays() {
  const auto now = layerc::core::MonotonicClock::now();
  publish_companion_snapshots(now);
  bool has_running_cooldown{};

  for (auto &session : session_registry_.sessions) {
    apply_automation_overrides(*session);
    const bool hidden_for_edit = live_edit_host_.session != nullptr &&
                                 !session->is_global() &&
                                 session->game_window == live_edit_host_.session->target;
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) ||
         IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(session_registry_.active_inputs.begin(), session_registry_.active_inputs.end(),
                  session.get()) != session_registry_.active_inputs.end();
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
      session->overlays.render_on_rect(overlay_target(), states, now);
    else
      session->overlays.render_for(session->game_window, states, now);
  }

  visual_capture_runtime_.sync_exclusions();

  if (has_running_cooldown && !overlay_runtime_.timer_running) {
    SetTimer(window_, overlay_render_timer, 16, nullptr);
    overlay_runtime_.timer_running = true;
  } else if (!has_running_cooldown && overlay_runtime_.timer_running) {
    KillTimer(window_, overlay_render_timer);
    overlay_runtime_.timer_running = false;
  }
}

void DaemonApp::publish_companion_snapshots(const layerc::core::TimePoint now) {
  if (companion_host_.transport.wait_handle() == nullptr)
    return;
  const auto foreground = current_companion_sessions();
  auto *session = foreground.empty() ? nullptr : foreground.front();
  if (session == nullptr)
    return;
  (void)session->automation.snapshot(now);
  const auto session_key =
      layerc::platform::window_session_key(session->game_window);
  const auto json = layerc::core::build_companion_snapshot(
      session->automation.profile().id, session_key, session->automation.profile().panels,
      session->automation.profile().cooldowns, session->automation.snapshot_buffer(),
      session->modules.get<layerc::core::CompanionModule>().profile, now);
  if (!json.empty())
    (void)companion_host_.transport.publish(json);
}

void DaemonApp::poll_gamebar_transport() {
  bool reload_needed{};
  for (auto &update : companion_host_.transport.poll()) {
    const bool accepted = apply_companion_preferences(update.json);
    companion_host_.transport.complete_preference_update(update.request_id, accepted);
    reload_needed = reload_needed || accepted;
  }
  if (reload_needed)
    reload_profiles();
}

bool DaemonApp::apply_companion_preferences(const std::string_view json) {
  for (const auto &source : session_registry_.sources) {
    const auto parsed =
        layerc::core::parse_companion_profile_json(json, source.profile);
    const auto *profile = std::get_if<layerc::core::CompanionProfile>(&parsed);
    if (profile == nullptr)
      continue;
    if (!layerc::platform::may_write_primary(source.modules.get<layerc::core::VisualModule>().state) ||
        !layerc::platform::may_write_primary(source.modules.get<layerc::core::AutomationModule>().state))
      return false;
    return layerc::platform::write_profile_bundle(
               source.path, source.profile, source.modules.get<layerc::core::VisualModule>().profile,
               source.modules.get<layerc::core::AutomationModule>().profile, *profile)
        .ok();
  }
  return false;
}

} // namespace layerc::daemon
