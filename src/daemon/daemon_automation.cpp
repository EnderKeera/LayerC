#include "daemon_app.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <string>

namespace layerc::daemon {
namespace {
using layerc::core::AutomationEvent;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationSession;
using layerc::core::Milliseconds;
using layerc::core::TimePoint;

} // namespace

void DaemonApp::reconcile_automation_runtime() {
  const auto now = layerc::core::MonotonicClock::now();
  // A new or reconfigured runtime starts with fresh session-scoped state, so
  // it seeds cooldown edges and replays session_started.
  const auto start_session = [&](Session &session) {
    poll_automation_cooldown_transitions(session, now, false);
    process_automation_event(
        session,
        AutomationSession::event(AutomationEventKind::session_started, {}, now));
  };
  for (auto &session : session_registry_.sessions) {
    if (!session->has_enabled_automations()) {
      session->automation_runtime.reset();
      continue;
    }
    const auto source = std::ranges::find(session_registry_.sources, session->source_key,
                                          &ProfileSource::key);
    if (source == session_registry_.sources.end() ||
        !source->modules.automation.profile.has_value()) {
      session->automation_runtime.reset();
      continue;
    }
    if (!session->automation_runtime) {
      session->automation_revision = source->automation_revision;
      if (session->start(*source->modules.automation.profile,
                         source->automation_state, now, nullptr,
                         automation_runtime_host_.trace_subscribed))
        mark_overlays_dirty();
      publish_automation_trace(*session);
      synchronize_automation_state(*session, now);
    } else if (session->automation_revision != source->automation_revision) {
      // The revision gate replaced a deep AutomationProfile compare plus a
      // cooldown-group compare that used to run on every reconcile: now they
      // run only after the source's automation-relevant content changed, and
      // they still decide whether a full reconfigure is needed.
      session->automation_revision = source->automation_revision;
      if (session->automation_runtime->profile() !=
              *source->modules.automation.profile ||
          !std::ranges::equal(
              session->automation_runtime->cooldown_groups(),
              session->profile.cooldown_groups)) {
        session->automation_runtime->reconfigure(
            *source->modules.automation.profile,
            session->profile.cooldown_groups);
        session->automation_runtime->apply_persistent_values(
            source->automation_state);
        // Reconfigure resets session-scoped state (variables, timers, edge
        // monitors), so replay session_started like a fresh runtime: rules
        // keyed on it re-run against the edited profile instead of waiting
        // for the next session.
        start_session(*session);
      }
    }
  }
  const bool active = std::ranges::any_of(session_registry_.sessions, [](const auto &session) {
    return session != nullptr && session->automation_runtime != nullptr;
  });
  automation_runtime_host_.publish_activity(active);
  schedule_automation_timer();
}

void DaemonApp::quiesce_automation_runtime() noexcept {
  if (automation_runtime_host_.timer_running && window_ != nullptr)
    KillTimer(window_, automation_timer);
  automation_runtime_host_.timer_running = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    process_automation_event(
        *session,
        AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
    synchronize_automation_state(*session, now);
    session->automation_runtime.reset();
  }
  // Shutdown flush: force bypasses the debounce deadline.
  for (auto &source : session_registry_.sources)
    if (source.modules.automation.profile.has_value())
      (void)layerc::platform::checkpoint_automation_state(
          source.path, *source.modules.automation.profile,
          source.automation_state, source.automation_state_due,
          layerc::core::MonotonicClock::now(), /*force=*/true);
  automation_runtime_host_.publish_activity(false);
}

void DaemonApp::retire_automation_session(Session &session,
                                          const TimePoint now) {
  if (!session.automation_runtime)
    return;
  process_automation_event(
      session,
      AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
  synchronize_automation_state(session, now);
  const auto source = std::ranges::find(session_registry_.sources, session.source_key,
                                        &ProfileSource::key);
  if (source != session_registry_.sources.end() &&
      source->modules.automation.profile.has_value())
    (void)layerc::platform::checkpoint_automation_state(
        source->path, *source->modules.automation.profile,
        source->automation_state, source->automation_state_due, now,
        /*force=*/true);
  session.automation_runtime.reset();
}
void DaemonApp::schedule_automation_timer() {
  if (window_ == nullptr)
    return;
  const auto now = layerc::core::MonotonicClock::now();
  std::optional<TimePoint> next;
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    next = session->next_deadline(now, next);
  }
  for (const auto &source : session_registry_.sources) {
    if (source.automation_state_due.has_value() &&
        (!next.has_value() || *source.automation_state_due < *next))
      next = source.automation_state_due;
  }
  if (!next.has_value()) {
    if (automation_runtime_host_.timer_running)
      KillTimer(window_, automation_timer);
    automation_runtime_host_.timer_running = false;
    return;
  }
  const auto remaining =
      std::max(Milliseconds(1), std::chrono::ceil<Milliseconds>(*next - now));
  const auto delay = static_cast<UINT>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<UINT>::max()));
  // SetTimer replaces a running timer with the same id.
  automation_runtime_host_.timer_running =
      SetTimer(window_, automation_timer, delay, nullptr) != 0;
}
void DaemonApp::poll_automation_timer() {
  if (automation_runtime_host_.timer_running)
    KillTimer(window_, automation_timer);
  automation_runtime_host_.timer_running = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    if (session->poll(now))
      mark_overlays_dirty();
    publish_automation_trace(*session);
    synchronize_automation_state(*session, now);
  }
  for (auto &source : session_registry_.sources) {
    if (!source.modules.automation.profile.has_value())
      continue;
    const auto stored = layerc::platform::checkpoint_automation_state(
        source.path, *source.modules.automation.profile,
        source.automation_state, source.automation_state_due, now);
    if (std::holds_alternative<layerc::platform::CheckpointSaved>(stored))
      acknowledge_automation_state(source);
  }
  schedule_automation_timer();
}

void DaemonApp::poll_automation_cooldown_transitions(Session &session,
                                                     const TimePoint now,
                                                     const bool emit) {
  if (!session.automation_runtime)
    return;
  if (session.poll_ready(now, emit))
    mark_overlays_dirty();
  if (emit) {
    publish_automation_trace(session);
    synchronize_automation_state(session, now);
    schedule_automation_timer();
  }
}
void DaemonApp::publish_automation_trace(Session &session) {
  if (!automation_runtime_host_.trace_subscribed ||
      !live_edit_host_.authenticated ||
      session.automation_runtime->trace().empty())
    return;
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::automation_trace_event;
  message.target_window =
      reinterpret_cast<std::uintptr_t>(session.game_window);
  message.profile_key = session.source_key;
  constexpr std::size_t max_chunks_per_event = 4U;
  for (std::size_t chunk{}; chunk < max_chunks_per_event &&
                            !session.automation_runtime->trace().empty();
       ++chunk) {
    const auto &trace = session.automation_runtime->trace();
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
      if (!live_edit_host_.channel.send(message))
        break;
      session.automation_runtime->consume_trace(1U);
      continue;
    }
    message.text = layerc::core::export_layerc_trace(
        std::span<const layerc::core::AutomationTraceEntry>(trace.data(),
                                                            selected));
    if (!live_edit_host_.channel.send(message))
      break;
    session.automation_runtime->consume_trace(selected);
  }
}

void DaemonApp::process_automation_event(Session &session,
                                         const AutomationEvent &event) {
  if (!session.automation_runtime)
    return;
  if (session.process(event))
    mark_overlays_dirty();
  publish_automation_trace(session);
  synchronize_automation_state(session, event.timestamp);
  schedule_automation_timer();
}

void DaemonApp::acknowledge_automation_state(const ProfileSource &source) {
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime || session->source_key != source.key ||
        session->automation_runtime->snapshot().persistent_values !=
            source.automation_state)
      continue;
    session->automation_runtime->mark_persistent_saved();
  }
}
void DaemonApp::synchronize_automation_state(Session &session,
                                             const TimePoint now) {
  if (!session.automation_runtime ||
      !session.automation_runtime->persistent_dirty())
    return;
  const auto source = std::ranges::find(session_registry_.sources, session.source_key,
                                        &ProfileSource::key);
  if (source == session_registry_.sources.end())
    return;
  source->automation_state =
      session.automation_runtime->snapshot().persistent_values;
  // Shared 1 s debounce (core::automation_checkpoint_arm): the first
  // mutation in a burst arms the deadline; later mutations keep it instead
  // of postponing the checkpoint forever under continuous input.
  layerc::core::automation_checkpoint_arm(source->automation_state_due, now);
  for (auto &other : session_registry_.sessions) {
    if (other.get() == &session || !other->automation_runtime ||
        other->source_key != session.source_key)
      continue;
    (void)session.sync_persistent_to(*other);
  }
}

} // namespace layerc::daemon
