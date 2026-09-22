// X11 input capability (V2.6.1): profile sessions driven by passive XI2 raw
// input, registered with the daemon kernel as Capability::input.
#include "input_service.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::core::AutomationEffect;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationPersistentValues;
using layerc::core::AutomationProfile;
using layerc::core::TimePoint;
using layerc::core::VisualProfile;
using layerc::core::VisualTriggerRequest;

struct LoadedVisual {
  std::optional<VisualProfile> profile;
  std::string diagnostic;
};

struct LoadedAutomation {
  std::optional<AutomationProfile> profile;
  AutomationPersistentValues state;
  std::string diagnostic;
};

[[nodiscard]] LoadedVisual
load_visual(const layerc::platform::StoredProfile &source) {
  LoadedVisual result;
  auto sidecar =
      layerc::platform::load_visual_sidecar(source.path, source.profile);
  if (!sidecar.profile.has_value()) {
    if (sidecar.state == layerc::platform::VisualSidecarState::invalid)
      result.diagnostic = source.path.string() + ": " + sidecar.error.message;
    return result;
  }
  if (layerc::core::has_enabled_visual_assignments(*sidecar.profile))
    result.profile = std::move(sidecar.profile);
  return result;
}

[[nodiscard]] bool enabled(const AutomationProfile &profile) {
  return std::ranges::any_of(profile.automations, [](const auto &automation) {
    return automation.enabled && !automation.rules.empty();
  });
}

[[nodiscard]] LoadedAutomation
load_automation(const layerc::platform::StoredProfile &source) {
  LoadedAutomation result;
  auto sidecar = layerc::platform::load_automation_sidecar(source.path,
                                                           source.profile);
  if (!sidecar.profile.has_value()) {
    if (sidecar.state == layerc::platform::AutomationSidecarState::invalid)
      result.diagnostic = source.path.string() + ": " + sidecar.error.message;
    return result;
  }

  auto state = layerc::platform::load_automation_state(source.path,
                                                       *sidecar.profile);
  result.state = std::move(state.values);
  if (state.error.has_value())
    result.diagnostic = source.path.string() + ": " + state.error->message;
  if (enabled(*sidecar.profile)) result.profile = std::move(sidecar.profile);
  return result;
}

[[nodiscard]] bool
same_visual(const InputSession &session,
            const std::optional<VisualProfile> &profile) noexcept {
  return session.visual_profile == profile;
}

[[nodiscard]] bool same_automation(
    const InputSession &session,
    const std::optional<AutomationProfile> &profile) noexcept {
  if (!profile.has_value()) return session.automation == nullptr;
  return session.automation != nullptr &&
         session.automation->profile() == *profile;
}

} // namespace

InputSession::InputSession(
    layerc::platform::StoredProfile source, const std::uint64_t window,
    const bool global, std::optional<VisualProfile> loaded_visual_profile,
    std::optional<AutomationProfile> automation_profile,
    AutomationPersistentValues automation_state)
    : source_path(std::move(source.path)), profile(std::move(source.profile)),
      window(window), global(global), matcher(profile.bindings),
      engine(profile.cooldowns, profile.cooldown_groups),
      visual_profile(std::move(loaded_visual_profile)) {
  if (visual_profile.has_value())
    visual_triggers =
        std::make_unique<layerc::core::VisualTriggerEngine>(*visual_profile);
  if (!automation_profile.has_value()) return;
  automation = std::make_unique<layerc::core::AutomationRuntime>(
      std::move(*automation_profile), std::move(automation_state),
      profile.cooldown_groups);
  const auto now = layerc::core::MonotonicClock::now();
  poll_cooldown_transitions(now, false);
  process_automation_event(
      automation_event(AutomationEventKind::session_started, {}, now));
}

layerc::core::AutomationEvent InputSession::automation_event(
    const AutomationEventKind kind, std::string source_id, const TimePoint now,
    const std::optional<std::uint64_t> origin) {
  return {.kind = kind,
          .timestamp = now,
          .source_id = std::move(source_id),
          .origin_token = origin.value_or(automation_origin_sequence++),
          .fields = {}};
}

void InputSession::process_automation_event(
    const layerc::core::AutomationEvent &event) {
  if (!automation) return;
  // Validation, staged commit, and edge computation live in
  // core::automation_session; this keeps only session event plumbing.
  const auto result = automation->process(
      event,
      [&](const AutomationEffect &effect) {
        return layerc::core::validate_automation_effect(
            effect, profile.panels, profile.cooldowns,
            profile.cooldown_groups);
      },
      [&](const std::span<const AutomationEffect> effects) {
        return layerc::core::commit_automation_cooldown_effects(
            engine, effects, profile.cooldowns, profile.cooldown_groups,
            event.timestamp, event.origin_token);
      });
  (void)result;
  poll_cooldown_transitions(event.timestamp, false);
  synchronize_automation_state(event.timestamp);
}

void InputSession::poll_cooldown_transitions(const TimePoint now,
                                             const bool emit) {
  if (!automation) return;
  engine.snapshot(now, cooldown_snapshot);
  const auto ready = layerc::core::poll_automation_ready_transitions(
      automation->edges(), cooldown_snapshot, profile.cooldown_groups);
  if (!emit) return;
  for (const auto &id : ready.cooldowns)
    process_automation_event(
        automation_event(AutomationEventKind::cooldown_ready, id, now));
  for (const auto &id : ready.groups)
    process_automation_event(
        automation_event(AutomationEventKind::group_ready, id, now));
}

void InputSession::synchronize_automation_state(const TimePoint now) {
  if (automation && automation->persistent_dirty())
    layerc::core::automation_checkpoint_arm(automation_state_due, now);
}

void InputSession::checkpoint_automation_state(const TimePoint now,
                                               const bool force) {
  synchronize_automation_state(now);
  if (!automation || !layerc::core::automation_checkpoint_ready(
                          automation_state_due, now, force))
    return;
  const bool stored = layerc::platform::write_automation_state(
      source_path, automation->profile(),
      automation->snapshot().persistent_values);
  if (stored) automation->mark_persistent_saved();
  layerc::core::automation_checkpoint_report(automation_state_due, now,
                                              stored);
}

void InputSession::trigger_requests(
    const std::span<const VisualTriggerRequest> requests, const TimePoint now,
    std::vector<std::string> &triggered) {
  for (const auto &request : requests) {
    const auto origin = automation_origin_sequence++;
    for (const auto &id :
         engine.trigger_with_propagation(request.cooldown_id, now, origin)) {
      triggered.push_back(id);
      if (visual_triggers) visual_triggers->on_cooldown_triggered(id);
      if (automation)
        process_automation_event(automation_event(
            AutomationEventKind::cooldown_triggered, id, now, origin));
    }
  }
}

std::vector<std::string>
InputSession::feed(const layerc::core::InputEvent &event, const TimePoint now) {
  if (automation) {
    auto source = std::string(layerc::core::input_device_label(event.token.device)) +
                  ":" + std::to_string(event.token.code);
    if (event.token.device == layerc::core::InputDevice::controller)
      source += ":" + std::to_string(event.token.controller_index);
    process_automation_event(automation_event(
        event.pressed ? AutomationEventKind::input_pressed
                      : AutomationEventKind::input_released,
        std::move(source), now));
  }

  std::vector<std::string> triggered;
  for (const auto &cooldown_id : matcher.process(event)) {
    if (visual_triggers) {
      const auto requests = visual_triggers->on_input(cooldown_id, now);
      trigger_requests(requests, now, triggered);
    } else {
      const VisualTriggerRequest request{
          cooldown_id, layerc::core::VisualTriggerSource::direct_input};
      trigger_requests(std::span{&request, 1U}, now, triggered);
    }
  }
  return triggered;
}

std::vector<std::string> InputSession::feed_measurements(
    const std::span<const layerc::core::ObservationMeasurement> measurements,
    const TimePoint now) {
  std::vector<std::string> triggered;
  if (!visual_triggers) return triggered;

  engine.snapshot(now, cooldown_snapshot);
  if (automation) {
    for (const auto &measurement : measurements) {
      auto event = automation_event(AutomationEventKind::visual_observation,
                                    measurement.observation_id, now);
      event.fields.emplace(
          "available", layerc::core::AutomationValue(
                           measurement.availability ==
                           layerc::core::CaptureAvailability::available));
      event.fields.emplace("matched",
                           layerc::core::AutomationValue(measurement.matched));
      event.fields.emplace(
          "confidence",
          layerc::core::AutomationValue(
              layerc::core::ConfidenceValue{measurement.confidence}));
      process_automation_event(event);
    }
  }
  const auto requests =
      visual_triggers->on_measurements(measurements, cooldown_snapshot, now);
  trigger_requests(requests, now, triggered);
  return triggered;
}

void InputSession::poll_automation(const TimePoint now) {
  if (!automation) return;
  poll_cooldown_transitions(now, true);
  (void)automation->expire_overlay_overrides(now);
  for (const auto &event : automation->due_timer_events(now))
    process_automation_event(event);
  checkpoint_automation_state(now, false);
}

std::optional<TimePoint>
InputSession::next_automation_deadline(const TimePoint now) {
  if (!automation) return std::nullopt;
  engine.snapshot(now, cooldown_snapshot);
  return layerc::core::next_automation_deadline(
      *automation, cooldown_snapshot, automation_state_due);
}

void InputSession::stop_automation(const TimePoint now) {
  if (!automation) return;
  process_automation_event(
      automation_event(AutomationEventKind::session_stopping, {}, now));
  checkpoint_automation_state(now, true);
  automation.reset();
  automation_state_due.reset();
}

void InputSession::checkpoint_automation_now(const TimePoint now) {
  checkpoint_automation_state(now, true);
}

void X11InputService::attach(layerc::platform::X11Display &display) noexcept {
  display_ = &display;
}

void X11InputService::refresh(
    const std::span<const layerc::platform::StoredProfile> profiles,
    const layerc::platform::X11WindowScan &scan) {
  auto previous = std::move(sessions_);
  std::vector<InputSession> next;
  automation_diagnostics_.clear();
  visual_diagnostics_.clear();
  for (const auto &source : profiles) {
    const auto &profile = source.profile;
    if (!profile.enabled) continue;
    auto visual = load_visual(source);
    if (!visual.diagnostic.empty())
      visual_diagnostics_.push_back(std::move(visual.diagnostic));
    if (profile.bindings.empty() && !visual.profile.has_value()) continue;
    const auto previous_session =
        std::ranges::find(previous, source.path, &InputSession::source_path);
    const auto previous_window = previous_session == previous.end()
                                     ? 0 : previous_session->window;
    const auto *window =
        layerc::platform::match_window(scan, profile.game, previous_window);
    if (window == nullptr) continue;

    auto loaded = load_automation(source);
    if (!loaded.diagnostic.empty())
      automation_diagnostics_.push_back(std::move(loaded.diagnostic));
    // Foreground eligibility is runtime state, not profile identity: a
    // background game keeps its automation profile and cooldown engine so
    // focus-away/back never reconstructs the session. Dispatch still gates
    // event feeding on session.active below.
    const bool active = profile.game.global || window->active;

    const auto retained = std::ranges::find_if(
        previous, [&](const InputSession &session) {
          const bool same_target =
              profile.game.global || session.window == window->window;
          return same_target && session.source_path == source.path &&
                 session.profile == profile &&
                 same_visual(session, visual.profile) &&
                 same_automation(session, loaded.profile);
        });
    if (retained == previous.end()) {
      const auto stale = std::ranges::find(previous, source.path,
                                           &InputSession::source_path);
      if (stale != previous.end()) {
        stale->stop_automation(layerc::core::MonotonicClock::now());
        previous.erase(stale);
        if (loaded.profile.has_value()) {
          const auto state = layerc::platform::load_automation_state(
              source.path, *loaded.profile);
          loaded.state = std::move(state.values);
        }
      }
      next.emplace_back(source, window->window, profile.game.global,
                        std::move(visual.profile), std::move(loaded.profile),
                        std::move(loaded.state));
    } else {
      if (retained->automation && !retained->automation->persistent_dirty())
        retained->automation->apply_persistent_values(loaded.state);
      next.push_back(std::move(*retained));
      previous.erase(retained);
      next.back().window = window->window;
    }
    if (!active) next.back().matcher.clear();
    next.back().active = active;
  }
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : previous) session.stop_automation(now);
  sessions_ = std::move(next);
}

void X11InputService::stop_sessions(const TimePoint now) {
  for (auto &session : sessions_) session.stop_automation(now);
  sessions_.clear();
}

std::vector<layerc::runtime::SessionCapabilityRequest>
X11InputService::session_requests() const {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(sessions_.size());
  for (const auto &session : sessions_) {
    layerc::runtime::SessionCapabilityRequest request;
    request.window = layerc::runtime::WindowId{session.window};
    request.active = session.active;
    request.global = session.global;
    request.visual = !session.global && session.visual_profile.has_value();
    request.isolated_visual =
        request.visual &&
        session.profile.visual_runtime == layerc::core::VisualRuntime::isolated;
    request.profile_id = session.profile.id;
    requests.push_back(request);
  }
  return requests;
}

X11InputDispatch X11InputService::dispatch() {
  X11InputDispatch result;
  if (!source_.has_value()) return result;
  auto poll = source_->poll();
  result.events = poll.events.size();
  result.windows_changed = poll.windows_changed;
  result.devices_changed = poll.devices_changed;
  if (poll.events.empty() || sessions_.empty()) return result;
  const auto now = layerc::core::MonotonicClock::now();
  for (const auto &event : poll.events)
    for (auto &session : sessions_) {
      if (!session.global && !session.active) continue;
      auto triggered = session.feed(event, now);
      result.triggered.insert(result.triggered.end(),
                              std::make_move_iterator(triggered.begin()),
                              std::make_move_iterator(triggered.end()));
    }
  return result;
}

std::vector<std::string> X11InputService::unavailable() const {
  if (!source_.has_value()) return {"input: no profile session demands input"};
  return source_->unavailable();
}

std::size_t X11InputService::automation_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      sessions_, [](const auto &session) { return session.automation != nullptr; }));
}

void X11InputService::poll_automation(const TimePoint now) {
  for (auto &session : sessions_) session.poll_automation(now);
}

std::optional<TimePoint>
X11InputService::next_automation_deadline(const TimePoint now) {
  std::optional<TimePoint> next;
  for (auto &session : sessions_) {
    const auto due = session.next_automation_deadline(now);
    if (due.has_value() && (!next.has_value() || *due < *next)) next = due;
  }
  return next;
}

layerc::runtime::ServiceReconcileResult
X11InputService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::input))
    return {false, "input: service invoked without demand"};
  if (source_.has_value()) return {};
  if (display_ == nullptr || !display_->valid())
    return {false, "input: no X display for the input service"};
  std::string error;
  auto source = layerc::platform::X11InputSource::create(*display_, error);
  if (!source.has_value()) return {false, error};
  source_ = std::move(*source);
  return {};
}

void X11InputService::quiesce() noexcept {
  // Sessions outlive the XI2 subscription: zero input demand releases only
  // the device source so running cooldowns and automation runtimes survive.
  // Automation state is owned by X11AutomationService; session objects
  // are retired only by refresh() when their profile/window no longer
  // resolves.
  source_.reset();
  for (auto &session : sessions_) session.matcher.clear();
}

layerc::runtime::RuntimeResourceStats
X11InputService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  stats.handles = source_.has_value() ? 1U : 0U;
  stats.objects = static_cast<std::uint32_t>(
      std::min<std::size_t>(sessions_.size(), 0xFFFFFFFFU));
  return stats;
}

layerc::runtime::ServiceReconcileResult
X11AutomationService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::automation))
    return {false, "automation: service invoked without demand"};
  return {};
}

void X11AutomationService::quiesce() noexcept {
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : input_.sessions()) {
    try {
      session.checkpoint_automation_now(now);
    } catch (...) {
    }
  }
}

layerc::runtime::RuntimeResourceStats
X11AutomationService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  const auto now = layerc::core::MonotonicClock::now();
  try {
    for (auto &session : input_.sessions()) {
      if (!session.automation) continue;
      ++stats.objects;
      const auto &snapshot = session.automation->snapshot();
      stats.cache_bytes +=
          snapshot.variables.size() * sizeof(layerc::core::AutomationValue);
      stats.cache_bytes +=
          snapshot.timers.size() * sizeof(layerc::core::AutomationTimer);
      stats.cache_bytes += snapshot.overlay_overrides.size() *
                           sizeof(layerc::core::OverlayPropertyOverride);
      if (session.next_automation_deadline(now).has_value()) stats.timers = 1U;
    }
  } catch (...) {
  }
  return stats;
}

} // namespace layerc::daemon
