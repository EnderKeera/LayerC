#include "daemon_state.hpp"

#include "layerc/platform/platform_events.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace layerc::daemon {

bool SessionRegistry::invariant_holds() const noexcept {
  for (const auto *active : active_inputs) {
    const auto matches = std::ranges::count_if(
        sessions, [active](const auto &session) { return session.get() == active; });
    if (active == nullptr || matches != 1) {
      return false;
    }
  }

  for (const auto &session : sessions) {
    if (session == nullptr)
      return false;
    if (session->is_global())
      continue;
    const layerc::core::RuntimeInstanceKey key{
        .profile_key = session->source_key,
        .window = {reinterpret_cast<std::uintptr_t>(session->game_window)}};
    if (!instances.contains(key))
      return false;
  }

  for (const auto &key : instances.active()) {
    const auto matches = std::ranges::count_if(sessions, [&](const auto &session) {
      return session != nullptr && !session->is_global() &&
             session->source_key == key.profile_key &&
             layerc::core::WindowId{
                 reinterpret_cast<std::uintptr_t>(session->game_window)} ==
                 key.window;
    });
    if (matches != 1)
      return false;
  }
  return true;
}

VisualCaptureRuntime::~VisualCaptureRuntime() {
  // Hosts may signal the snapshot event until they are destroyed.
  captures_.clear();
}

bool VisualCaptureRuntime::ensure_snapshot_event() {
  if (!snapshot_event_.valid())
    snapshot_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  return snapshot_event_.valid();
}

bool VisualCaptureRuntime::open_activity_event() {
  if (!activity_event_.valid())
    activity_event_.reset(CreateEventW(
        nullptr, TRUE, FALSE,
        layerc::platform::visual_capture_active_event_name));
  return activity_event_.valid();
}

void VisualCaptureRuntime::publish_activity(const bool active) noexcept {
  if (!activity_event_.valid())
    return;
  if (active)
    SetEvent(activity_event_.get());
  else
    ResetEvent(activity_event_.get());
}

layerc::runtime::RuntimeResourceStats VisualCaptureRuntime::resources(
    const layerc::core::VisualRuntime runtime) const noexcept {
  std::uint32_t count{};
  std::uint32_t inline_count{};
  std::uint64_t decoded_assets{};
  for (const auto &[window, worker] : captures_) {
    (void)window;
    if (worker == nullptr) continue;
    if (worker->runtime() == layerc::core::VisualRuntime::inline_process)
      ++inline_count;
    if (worker->runtime() == runtime) ++count;
  }
  for (const auto *session : sessions_.active_inputs) {
    if (session == nullptr || session->game_window == nullptr ||
        !session->modules.get<layerc::core::VisualModule>().profile.has_value())
      continue;
    const auto capture = captures_.find(session->game_window);
    if (capture == captures_.end() || capture->second == nullptr ||
        capture->second->runtime() != runtime)
      continue;
    for (const auto &state :
         layerc::core::observations_of<layerc::core::TemplateState>(
             session->modules.get<layerc::core::VisualModule>().profile->observations)) {
      const auto available =
          std::numeric_limits<std::uint64_t>::max() - decoded_assets;
      decoded_assets += std::min<std::uint64_t>(
          available, static_cast<std::uint64_t>(state.references.size()));
    }
  }
  const auto assets = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      decoded_assets, std::numeric_limits<std::uint32_t>::max()));
  const std::uint32_t snapshot_handle =
      count != 0 && snapshot_event_.valid() &&
              (runtime == layerc::core::VisualRuntime::inline_process ||
               inline_count == 0)
          ? 1U
          : 0U;
  if (runtime == layerc::core::VisualRuntime::isolated) {
    return {.threads = count,
            .handles = count * 3U + snapshot_handle,
            .objects = count,
            .capture_sessions = count,
            .queues = count,
            .decoded_assets = assets};
  }
  return {.threads = count,
          .handles = snapshot_handle,
          .capture_sessions = count,
          .decoded_assets = assets};
}

OverlayRuntime *OverlayRuntime::callback_target_{};

OverlayRuntime::~OverlayRuntime() {
  stop();
}

void OverlayRuntime::start() {
  stop();
  assert(callback_target_ == nullptr &&
         "only one OverlayRuntime may own WinEvent callbacks");
  if (callback_target_ != nullptr)
    return;
  callback_target_ = this;
  foreground_hook_ = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &OverlayRuntime::event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  // Destroy notifications retire a closed match immediately; location changes
  // keep non-activating overlays aligned without waiting for the scan timer.
  location_hook_ = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
      &OverlayRuntime::event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  if (foreground_hook_ == nullptr && location_hook_ == nullptr)
    callback_target_ = nullptr;
}

void OverlayRuntime::stop() noexcept {
  if (callback_target_ == this)
    callback_target_ = nullptr;
  if (foreground_hook_ != nullptr) {
    UnhookWinEvent(foreground_hook_);
    foreground_hook_ = nullptr;
  }
  if (location_hook_ != nullptr) {
    UnhookWinEvent(location_hook_);
    location_hook_ = nullptr;
  }
}

layerc::runtime::RuntimeResourceStats
OverlayRuntime::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result{
      .timers = timer_running_ ? 1U : 0U};
  const auto count = [&](const OverlayCollection &overlays) {
    result += {.windows = static_cast<std::uint32_t>(overlays.window_count()),
               .objects = static_cast<std::uint32_t>(overlays.object_count())};
  };
  for (const auto &session : sessions_.sessions) count(session->overlays);

  return result;
}

void CALLBACK OverlayRuntime::event_proc(
    const HWINEVENTHOOK hook, const DWORD event, const HWND window,
    const LONG object_id, const LONG child_id, const DWORD event_thread,
    const DWORD event_time) {
  (void)hook;
  (void)child_id;
  (void)event_thread;
  (void)event_time;
  auto *const target = callback_target_;
  if (target == nullptr)
    return;
  if (event == EVENT_SYSTEM_FOREGROUND ||
      ((event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_LOCATIONCHANGE) &&
       object_id == OBJID_WINDOW)) {
    target->events_.on_overlay_event(event, window);
  }
}

} // namespace layerc::daemon
