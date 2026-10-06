#include "daemon_state.hpp"

#include "layerc/platform/platform_events.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {
namespace {
using layerc::core::AutomationEvent;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationSession;
using layerc::core::Milliseconds;
using layerc::core::TimePoint;

ProfileSource *find_source(std::vector<ProfileSource> &sources,
                           const std::string_view key) {
  const auto found = std::ranges::find(sources, key, &ProfileSource::key);
  return found == sources.end() ? nullptr : &*found;
}

} // namespace

AutomationRuntimeHost::~AutomationRuntimeHost() {
  publish_activity(false);
}

void AutomationRuntimeHost::publish_activity(const bool active) noexcept {
  if (active && !active_event_.valid())
    active_event_.reset(CreateEventW(
        nullptr, TRUE, FALSE, layerc::platform::automation_active_event_name));
  if (!active_event_.valid())
    return;
  if (active) {
    SetEvent(active_event_.get());
    return;
  }
  ResetEvent(active_event_.get());
  active_event_.reset();
}

layerc::runtime::RuntimeResourceStats
AutomationRuntimeHost::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result{
      .timers = timer_running_ ? 1U : 0U};
  for (const auto &session : sessions_.sessions) {
    if (!session->automation.runtime()) continue;
    const auto &snapshot = session->automation.runtime()->snapshot();
    result += {
        .objects = 1,
        .cache_bytes =
            snapshot.variables.size() * sizeof(layerc::core::AutomationValue) +
            snapshot.timers.size() * sizeof(layerc::core::AutomationTimer) +
            snapshot.overlay_overrides.size() *
                sizeof(layerc::core::OverlayPropertyOverride)};
  }
  return result;
}

layerc::runtime::ServiceReconcileResult
AutomationRuntimeHost::reconcile(const layerc::runtime::CapabilityDemand &) {
  const auto now = layerc::core::MonotonicClock::now();
  // A new or reconfigured runtime starts with fresh session-scoped state, so
  // it seeds cooldown edges and replays session_started.
  const auto start_session = [&](Session &session) {
    poll_cooldown_transitions(session, now, false);
    process_event(
        session,
        AutomationSession::event(AutomationEventKind::session_started, {}, now));
  };
  for (auto &session : sessions_.sessions) {
    if (!session->has_enabled_automations()) {
      session->automation.discard_runtime();
      continue;
    }
    auto *source = find_source(sessions_.sources, session->source_key);
    if (source == nullptr || !source->modules.automation.profile.has_value()) {
      session->automation.discard_runtime();
      continue;
    }
    const auto &profile = *source->modules.automation.profile;
    if (!session->automation.runtime()) {
      session->automation_revision = source->automation_revision;
      if (session->automation.start(profile,
                         persistence_.attach(source->path, profile, now)
                             .values,
                         now, nullptr, trace_subscribed))
        overlays_.mark_dirty();
      after_step(*session, now);
    } else if (session->automation_revision != source->automation_revision) {
      // The revision gate replaced a deep AutomationProfile compare plus a
      // cooldown-group compare that used to run on every reconcile: now they
      // run only after the source's automation-relevant content changed, and
      // they still decide whether a full reconfigure is needed.
      session->automation_revision = source->automation_revision;
      if (session->automation.runtime()->profile() != profile ||
          !std::ranges::equal(
              session->automation.runtime()->cooldown_groups(),
              session->automation.profile().cooldown_groups)) {
        session->automation.runtime()->reconfigure(
            profile, session->automation.profile().cooldown_groups);
        session->automation.runtime()->apply_persistent_values(
            persistence_.attach(source->path, profile, now).values);
        // Reconfigure resets session-scoped state (variables, timers, edge
        // monitors), so replay session_started like a fresh runtime: rules
        // keyed on it re-run against the edited profile instead of waiting
        // for the next session.
        start_session(*session);
      }
    }
  }
  const bool active = std::ranges::any_of(sessions_.sessions, [](const auto &session) {
    return session != nullptr && session->automation.runtime() != nullptr;
  });
  publish_activity(active);
  schedule_timer();
  return {};
}

void AutomationRuntimeHost::quiesce() noexcept {
  if (timer_running_ && window_ != nullptr)
    KillTimer(window_, automation_timer);
  timer_running_ = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : sessions_.sessions) {
    if (!session->automation.runtime())
      continue;
    process_event(
        *session,
        AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
    session->automation.discard_runtime();
  }
  // Shutdown flush: force bypasses the debounce deadline. A failed write
  // stays pending in the owner with its fault visible in diagnostics.
  acknowledge_saved(persistence_.checkpoint(now, true));
  publish_activity(false);
}

void AutomationRuntimeHost::retire_session(Session &session,
                                           const TimePoint now) {
  if (!session.automation.runtime())
    return;
  process_event(
      session,
      AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
  if (const auto *source =
          find_source(sessions_.sources, session.source_key))
    acknowledge_saved(
        persistence_.checkpoint(now, true, &source->path));
  session.automation.discard_runtime();
}
void AutomationRuntimeHost::schedule_timer() {
  if (window_ == nullptr)
    return;
  const auto now = layerc::core::MonotonicClock::now();
  std::optional<TimePoint> next = persistence_.next_deadline();
  for (auto &session : sessions_.sessions) {
    if (!session->automation.runtime())
      continue;
    next = session->automation.next_deadline(now, next);
  }
  if (!next.has_value()) {
    if (timer_running_)
      KillTimer(window_, automation_timer);
    timer_running_ = false;
    return;
  }
  const auto remaining =
      std::max(Milliseconds(1), std::chrono::ceil<Milliseconds>(*next - now));
  const auto delay = static_cast<UINT>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<UINT>::max()));
  // SetTimer replaces a running timer with the same id.
  timer_running_ =
      SetTimer(window_, automation_timer, delay, nullptr) != 0;
}
void AutomationRuntimeHost::poll_timer() {
  if (timer_running_)
    KillTimer(window_, automation_timer);
  timer_running_ = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : sessions_.sessions) {
    if (!session->automation.runtime())
      continue;
    if (session->automation.poll(now))
      overlays_.mark_dirty();
    after_step(*session, now);
  }
  acknowledge_saved(persistence_.checkpoint(now));
  schedule_timer();
}

void AutomationRuntimeHost::poll_cooldown_transitions(Session &session,
                                                      const TimePoint now,
                                                      const bool emit) {
  if (!session.automation.runtime())
    return;
  if (session.automation.poll_ready(now, emit))
    overlays_.mark_dirty();
  if (emit)
    after_step(session, now);
}
void AutomationRuntimeHost::publish_trace(Session &session) {
  if (!trace_subscribed ||
      !live_edit_.authenticated ||
      session.automation.runtime()->trace().empty())
    return;
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::automation_trace_event;
  message.target_window =
      reinterpret_cast<std::uintptr_t>(session.game_window);
  message.profile_key = session.source_key;
  constexpr std::size_t max_chunks_per_event = 4U;
  for (std::size_t chunk{}; chunk < max_chunks_per_event &&
                            !session.automation.runtime()->trace().empty();
       ++chunk) {
    const auto &trace = session.automation.runtime()->trace();
    const auto selected =
        layerc::core::live_trace_prefix_count(message, trace);
    if (selected == 0U) {
      // A single entry that cannot fit a live frame is replaced by an
      // explicit omission marker so the editor still sees progress.
      auto omitted = trace.front();
      omitted.phase = "error";
      omitted.detail = "source trace entry omitted because it exceeded the "
                       "live frame limit";
      omitted.source_event.reset();
      message.text = layerc::core::export_layerc_trace(
          std::span<const layerc::core::AutomationTraceEntry>(&omitted, 1U));
      if (!live_edit_.channel.send(message))
        break;
      session.automation.runtime()->consume_trace(1U);
      continue;
    }
    message.text = layerc::core::export_layerc_trace(
        std::span<const layerc::core::AutomationTraceEntry>(trace.data(),
                                                            selected));
    if (!live_edit_.channel.send(message))
      break;
    session.automation.runtime()->consume_trace(selected);
  }
}

void AutomationRuntimeHost::process_event(Session &session,
                                          const AutomationEvent &event) {
  if (!session.automation.runtime())
    return;
  if (session.automation.process(event))
    overlays_.mark_dirty();
  after_step(session, event.timestamp);
}

void AutomationRuntimeHost::process_input_value(Session &session,
                                                std::string source,
                                                const double value,
                                                const TimePoint now,
                                                const bool momentary) {
  if (auto event =
          session.automation.input_value_event(std::move(source), value, now, momentary))
    process_event(session, *event);
}

void AutomationRuntimeHost::after_step(Session &session, const TimePoint now) {
  publish_trace(session);
  synchronize_state(session, now);
  schedule_timer();
}

void AutomationRuntimeHost::acknowledge_saved(
    const std::span<const std::filesystem::path> saved) {
  for (const auto &path : saved) {
    const auto *entry = persistence_.find(path);
    if (entry == nullptr)
      continue;
    for (auto &session : sessions_.sessions) {
      const auto *source =
          find_source(sessions_.sources, session->source_key);
      if (!session->automation.runtime() || source == nullptr ||
          source->path != path ||
          session->automation.runtime()->persistent_values() != entry->values)
        continue;
      session->automation.runtime()->mark_persistent_saved();
    }
  }
}
void AutomationRuntimeHost::synchronize_state(Session &session,
                                              const TimePoint now) {
  if (!session.automation.runtime() ||
      !session.automation.runtime()->persistent_dirty())
    return;
  const auto *source =
      find_source(sessions_.sources, session.source_key);
  if (source == nullptr)
    return;
  // The owner arms the shared debounce on the first change of a burst.
  persistence_.publish(
      source->path, session.automation.runtime()->persistent_values(), now);
  for (auto &other : sessions_.sessions) {
    if (other.get() == &session || !other->automation.runtime() ||
        other->source_key != session.source_key)
      continue;
    (void)session.automation.sync_persistent_to(other->automation);
  }
}

} // namespace layerc::daemon
