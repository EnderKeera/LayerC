#include "daemon_state.hpp"
#include "companion_session_selection.hpp"

#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

namespace layerc::daemon {

CompanionHost::~CompanionHost() { quiesce(); }

layerc::runtime::ServiceReconcileResult
CompanionHost::reconcile(const layerc::runtime::CapabilityDemand &) {
  if (!transport_.start()) {
    schedule_publication(std::nullopt, layerc::core::MonotonicClock::now());
    return layerc::runtime::ServiceReconcileResult::failed(
        "Game Bar transport could not start");
  }
  // Seed the presentation timer from the producer itself instead of waiting
  // for a native overlay render.
  publish_snapshot(layerc::core::MonotonicClock::now());
  return {};
}

void CompanionHost::quiesce() noexcept {
  schedule_publication(std::nullopt, layerc::core::MonotonicClock::now());
  transport_.stop();
  set_active_session_count(0);
}

void CompanionHost::schedule_publication(
    const std::optional<layerc::core::TimePoint> next,
    const layerc::core::TimePoint now) noexcept {
  if (timer_running_ && window_ != nullptr)
    KillTimer(window_, companion_snapshot_timer);
  timer_running_ = false;
  if (!next || window_ == nullptr) return;
  const auto delay = std::chrono::ceil<layerc::core::Milliseconds>(*next - now);
  const auto milliseconds = static_cast<UINT>(std::clamp<std::int64_t>(
      delay.count(), 1, USER_TIMER_MAXIMUM));
  timer_running_ = SetTimer(window_, companion_snapshot_timer, milliseconds, nullptr) != 0;
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
  return {.timers = timer_running_ ? 1U : 0U,
          .handles =
              static_cast<std::uint32_t>(transport_.resource_handle_count()),
          .objects = owned_sessions};
}

void CompanionHost::publish_snapshot(const layerc::core::TimePoint now) {
  // Every publication rebuilds the producer-owned deadline from scratch, so
  // a stale timer never survives a failed or empty build.
  schedule_publication(std::nullopt, now);
  if (transport_.wait_handle() == nullptr)
    return;
  const auto foreground = current_sessions();
  auto *session = foreground.empty() ? nullptr : foreground.front();
  if (session == nullptr)
    return;
  (void)session->automation.snapshot(now);
  // Typed rows use the same automation values, overrides and flash epoch as
  // the session's native overlays.
  std::optional<layerc::core::AutomationRuntimeSnapshot> runtime_snapshot;
  if (session->automation.runtime())
    runtime_snapshot = session->automation.runtime()->snapshot();
  const auto *automation = runtime_snapshot ? &*runtime_snapshot
      : (session->presentation_defaults ? &*session->presentation_defaults : nullptr);
  const auto session_key =
      layerc::platform::window_session_key(session->game_window);
  const auto built = layerc::core::build_companion_snapshot_report(
      session->automation.profile().id, session_key,
      session->automation.profile().panels,
      session->automation.profile().cooldowns,
      session->automation.snapshot_buffer(),
      session->modules.get<layerc::core::CompanionModule>().profile, now,
      {.automation = automation, .presentation_epoch = session->presentation_epoch});
  if (built.json.empty()) {
    snapshot_fault_ = L"Game Bar snapshot not published: invalid or over 64 KiB";
    return;
  }
  snapshot_fault_ = built.omitted_items == 0 ? std::wstring{}
      : L"Game Bar snapshot omitted " + std::to_wstring(built.omitted_items) +
            L" item rows to fit 64 KiB";
  if (!transport_.publish(built.json)) return;
  schedule_publication(built.next_wakeup, now);
  if (built.next_wakeup && !timer_running_)
    snapshot_fault_ = L"Game Bar snapshot timer could not be scheduled";
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
