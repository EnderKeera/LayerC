#include "daemon_state.hpp"
#include "companion_session_selection.hpp"

#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <variant>

namespace layerc::daemon {

layerc::runtime::ServiceReconcileResult
CompanionHost::reconcile(const layerc::runtime::CapabilityDemand &) {
  return transport_.start()
             ? layerc::runtime::ServiceReconcileResult{}
             : layerc::runtime::ServiceReconcileResult::failed(
                   "Game Bar transport could not start");
}

void CompanionHost::quiesce() noexcept {
  transport_.stop();
  set_active_session_count(0);
}

void CompanionHost::set_active_session_count(const std::size_t count) noexcept {
  active_session_count_ = static_cast<std::uint32_t>(std::min<std::size_t>(
      count, std::numeric_limits<std::uint32_t>::max()));
}

layerc::runtime::RuntimeResourceStats CompanionHost::resources() const noexcept {
  const auto owned_sessions =
      layerc::gamebar::GameBarTransport::current_process_is_expected_package()
          ? active_session_count_
          : 0U;
  return {.handles =
              static_cast<std::uint32_t>(transport_.resource_handle_count()),
          .objects = owned_sessions};
}

void CompanionHost::publish_snapshot(const layerc::core::TimePoint now) {
  if (transport_.wait_handle() == nullptr)
    return;
  const auto foreground = current_sessions();
  auto *session = foreground.empty() ? nullptr : foreground.front();
  if (session == nullptr)
    return;
  (void)session->automation.snapshot(now);
  const auto session_key =
      layerc::platform::window_session_key(session->game_window);
  const auto json = layerc::core::build_companion_snapshot(
      session->automation.profile().id, session_key,
      session->automation.profile().panels,
      session->automation.profile().cooldowns,
      session->automation.snapshot_buffer(),
      session->modules.get<layerc::core::CompanionModule>().profile, now);
  if (!json.empty())
    (void)transport_.publish(json);
}

void CompanionHost::poll() {
  bool reload_needed{};
  for (auto &update : transport_.poll()) {
    const bool accepted = apply_preferences(update.json);
    transport_.complete_preference_update(update.request_id, accepted);
    reload_needed = reload_needed || accepted;
  }
  if (reload_needed)
    control_.reload_profiles();
}

bool CompanionHost::apply_preferences(const std::string_view json) {
  for (const auto &source : sessions_.sources) {
    const auto parsed =
        layerc::core::parse_companion_profile_json(json, source.profile);
    const auto *profile = std::get_if<layerc::core::CompanionProfile>(&parsed);
    if (profile == nullptr)
      continue;
    bool writable = true;
    source.modules.for_each([&]<typename Module>(Module, const auto &loaded) {
      // The incoming companion replaces that module. Preserve every other
      // registered module only when safe, including future registry entries.
      if constexpr (!std::is_same_v<Module, layerc::core::CompanionModule>)
        writable = writable &&
                   layerc::platform::may_write_primary(loaded.state);
    });
    if (!writable)
      return false;
    return layerc::platform::write_profile_bundle(
               source.path, source.profile,
               source.modules.get<layerc::core::VisualModule>().profile,
               source.modules.get<layerc::core::AutomationModule>().profile,
               *profile)
        .ok();
  }
  return false;
}

std::vector<Session *> CompanionHost::current_sessions() {
  const auto foreground = queries_.foreground_sessions();
  const HWND foreground_window = GetAncestor(GetForegroundWindow(), GA_ROOT);
  DWORD foreground_process_id{};
  if (foreground_window != nullptr)
    GetWindowThreadProcessId(foreground_window, &foreground_process_id);

  std::vector<Session *> all_sessions;
  all_sessions.reserve(sessions_.sessions.size());
  for (const auto &session : sessions_.sessions)
    all_sessions.push_back(session.get());

  const bool gamebar_foreground =
      platform::is_gamebar_overlay_process(foreground_process_id);
  Session *preferred{};
  if (!gamebar_foreground) {
    const auto game = std::ranges::find_if(
        foreground, [](const Session *session) {
          return session != nullptr && !session->is_global();
        });
    if (game != foreground.end())
      last_game_foreground_ = (*game)->game_window;
  } else {
    const auto game = std::ranges::find(
        all_sessions, last_game_foreground_, &Session::game_window);
    if (game != all_sessions.end())
      preferred = *game;
  }
  auto selected =
      select_companion_sessions<Session>(foreground, all_sessions,
                                         gamebar_foreground);
  if (preferred != nullptr)
    std::ranges::rotate(selected, std::ranges::find(selected, preferred));
  set_active_session_count(selected.size());
  return selected;
}
} // namespace layerc::daemon
