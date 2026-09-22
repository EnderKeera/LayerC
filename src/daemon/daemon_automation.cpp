#include "daemon_app.hpp"

#include "layerc/platform/platform_events.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <string>

namespace layerc::daemon {
namespace {
using layerc::core::AutomationActionKind;
using layerc::core::AutomationEffect;
using layerc::core::AutomationEvent;
using layerc::core::AutomationEventKind;
using layerc::core::DurationValue;
using layerc::core::Milliseconds;
using layerc::core::TimePoint;

} // namespace

layerc::core::AutomationEvent DaemonApp::automation_event(
    const layerc::core::AutomationEventKind kind, std::string source_id,
    const layerc::core::TimePoint now) {
  return automation_event(kind, std::move(source_id), now,
                          automation_runtime_host_.next_origin());
}

layerc::core::AutomationEvent DaemonApp::automation_event(
    const layerc::core::AutomationEventKind kind, std::string source_id,
    const layerc::core::TimePoint now, const std::uint64_t origin_token) {
  return layerc::core::AutomationEvent{.kind = kind,
                                       .timestamp = now,
                                       .source_id = std::move(source_id),
                                       .origin_token = origin_token,
                                       .fields = {}};
}

void DaemonApp::reconcile_automation_runtime() {
  const auto now = layerc::core::MonotonicClock::now();
  bool overlays_changed{};
  for (auto &session : session_registry_.sessions()) {
    if (!session->has_enabled_automations()) {
      session->automation_runtime.reset();
      continue;
    }
    const auto source = std::ranges::find(session_registry_.sources(), session->source_key,
                                          &ProfileSource::key);
    if (source == session_registry_.sources().end() ||
        !source->modules.automation.profile.has_value()) {
      session->automation_runtime.reset();
      continue;
    }
    if (!session->automation_runtime) {
      session->automation_runtime =
          std::make_unique<layerc::core::AutomationRuntime>(
              *source->modules.automation.profile, source->automation_state,
              session->profile.cooldown_groups);
      session->automation_revision = source->automation_revision;
      session->automation_runtime->set_trace_enabled(
          automation_runtime_host_.trace_subscribed());
      (void)poll_automation_cooldown_transitions(*session, now, false);
      overlays_changed =
          process_automation_event(
              *session,
              automation_event(AutomationEventKind::session_started, {}, now)) ||
          overlays_changed;
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
        (void)poll_automation_cooldown_transitions(*session, now, false);
        overlays_changed =
            process_automation_event(
                *session,
                automation_event(AutomationEventKind::session_started, {},
                                 now)) ||
            overlays_changed;
      }
    }
  }
  const bool active = std::ranges::any_of(session_registry_.sessions(), [](const auto &session) {
    return session != nullptr && session->automation_runtime != nullptr;
  });
  automation_runtime_host_.publish_activity(active);
  schedule_automation_timer();
  if (overlays_changed)
    update_overlays();
}

void DaemonApp::quiesce_automation_runtime() noexcept {
  if (automation_runtime_host_.timer_running() && window_ != nullptr)
    KillTimer(window_, automation_timer);
  automation_runtime_host_.set_timer_running(false);
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : session_registry_.sessions()) {
    if (!session->automation_runtime)
      continue;
    (void)process_automation_event(
        *session,
        automation_event(AutomationEventKind::session_stopping, {}, now));
    synchronize_automation_state(*session, now);
    session->automation_runtime.reset();
  }
  for (auto &source : session_registry_.sources()) {
    // Shutdown flush: force bypasses the debounce deadline; the outcome
    // reporters keep the retry discipline identical to the timer path.
    if (!source.modules.automation.profile.has_value() ||
        !layerc::core::automation_checkpoint_ready(
            source.automation_state_due,
            layerc::core::MonotonicClock::now(), /*force=*/true))
      continue;
    const bool stored = layerc::platform::write_automation_state(
            source.path, *source.modules.automation.profile,
            source.automation_state);
    layerc::core::automation_checkpoint_report(
        source.automation_state_due, layerc::core::MonotonicClock::now(),
        stored);
  }
  automation_runtime_host_.publish_activity(false);
}

void DaemonApp::retire_automation_session(Session &session,
                                          const TimePoint now) {
  if (!session.automation_runtime)
    return;
  (void)process_automation_event(
      session,
      automation_event(AutomationEventKind::session_stopping, {}, now));
  synchronize_automation_state(session, now);
  const auto source = std::ranges::find(session_registry_.sources(), session.source_key,
                                        &ProfileSource::key);
  if (source != session_registry_.sources().end() &&
      source->automation_state_due.has_value() &&
      source->modules.automation.profile.has_value() &&
      layerc::platform::write_automation_state(
          source->path, *source->modules.automation.profile,
          source->automation_state))
    source->automation_state_due.reset();
  session.automation_runtime.reset();
  session.applied_overlay_overrides.clear();
}
void DaemonApp::schedule_automation_timer() {
  if (window_ == nullptr)
    return;
  const auto now = layerc::core::MonotonicClock::now();
  std::optional<TimePoint> next;
  for (auto &session : session_registry_.sessions()) {
    if (!session->automation_runtime)
      continue;
    session->cooldowns.snapshot(now, session->snapshot_buffer);
    next = layerc::core::next_automation_deadline(
        *session->automation_runtime, session->snapshot_buffer, next);
  }
  for (const auto &source : session_registry_.sources()) {
    if (source.automation_state_due.has_value() &&
        (!next.has_value() || *source.automation_state_due < *next))
      next = source.automation_state_due;
  }
  if (automation_runtime_host_.timer_running()) {
    KillTimer(window_, automation_timer);
    automation_runtime_host_.set_timer_running(false);
  }
  if (!next.has_value())
    return;
  const auto remaining =
      std::max(Milliseconds(1), std::chrono::ceil<Milliseconds>(*next - now));
  const auto delay = static_cast<UINT>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<UINT>::max()));
  automation_runtime_host_.set_timer_running(
      SetTimer(window_, automation_timer, delay, nullptr) != 0);
}
void DaemonApp::poll_automation_timer() {
  if (automation_runtime_host_.timer_running())
    KillTimer(window_, automation_timer);
  automation_runtime_host_.set_timer_running(false);
  const auto now = layerc::core::MonotonicClock::now();
  bool changed{};
  for (auto &session : session_registry_.sessions()) {
    if (!session->automation_runtime)
      continue;
    changed =
        poll_automation_cooldown_transitions(*session, now, true) || changed;
    changed = session->automation_runtime->expire_overlay_overrides(now) ||
              changed;
    for (const auto &event :
         session->automation_runtime->due_timer_events(now))
      changed = process_automation_event(*session, event) || changed;
    synchronize_automation_state(*session, now);
  }
  for (auto &source : session_registry_.sources()) {
    if (!source.modules.automation.profile.has_value() ||
        !layerc::core::automation_checkpoint_ready(
            source.automation_state_due, now))
      continue;
    const bool stored = layerc::platform::write_automation_state(
            source.path, *source.modules.automation.profile,
            source.automation_state);
    layerc::core::automation_checkpoint_report(source.automation_state_due,
                                                now, stored);
    if (stored) acknowledge_automation_state(source);
  }
  if (changed)
    update_overlays();
  schedule_automation_timer();
}

bool DaemonApp::poll_automation_cooldown_transitions(Session &session,
                                                     const TimePoint now,
                                                     const bool emit) {
  if (!session.automation_runtime)
    return false;
  // Edge computation lives in core::poll_automation_ready_transitions; this
  // keeps only the trace/live event emission glue.
  session.cooldowns.snapshot(now, session.snapshot_buffer);
  const auto ready = layerc::core::poll_automation_ready_transitions(
      session.automation_runtime->edges(), session.snapshot_buffer,
      session.profile.cooldown_groups);
  bool changed{};
  if (emit) {
    for (const auto &id : ready.cooldowns)
      changed = process_automation_event(
                    session,
                    automation_event(AutomationEventKind::cooldown_ready, id,
                                     now)) ||
                changed;
    for (const auto &id : ready.groups)
      changed = process_automation_event(
                    session,
                    automation_event(AutomationEventKind::group_ready, id,
                                     now)) ||
                changed;
  }
  return changed;
}
bool DaemonApp::validate_automation_effect(
    const Session &session, const AutomationEffect &effect) const {
  // Policy lives in core::validate_automation_effect; the daemon supplies
  // only its session target sets.
  return layerc::core::validate_automation_effect(
      effect, session.profile.panels, session.profile.cooldowns,
      session.profile.cooldown_groups);
}

bool DaemonApp::commit_automation_effects(
    Session &session, const std::span<const AutomationEffect> effects,
    const TimePoint now, const std::uint64_t origin_token) {
  return layerc::core::commit_automation_cooldown_effects(
      session.cooldowns, effects, session.profile.cooldowns,
      session.profile.cooldown_groups, now, origin_token);
}
bool DaemonApp::trigger_cooldown_with_automation(
    Session &session, const std::string_view cooldown_id, const TimePoint now) {
  const auto origin = automation_runtime_host_.next_origin();
  const auto triggered =
      session.cooldowns.trigger_with_propagation(cooldown_id, now, origin);
  if (triggered.empty())
    return false;

  bool changed = true;
  for (const auto &id : triggered) {
    if (session.visual_triggers)
      session.visual_triggers->on_cooldown_triggered(id);
    if (session.automation_runtime)
      changed = process_automation_event(
                    session,
                    automation_event(AutomationEventKind::cooldown_triggered,
                                     id, now, origin)) ||
                changed;
  }
  return changed;
}
bool DaemonApp::process_automation_event(Session &session,
                                         const AutomationEvent &event) {
  if (!session.automation_runtime)
    return false;
  const auto now = event.timestamp;
  const auto result = session.automation_runtime->process(
      event,
      [&](const AutomationEffect &effect) {
        return validate_automation_effect(session, effect);
      },
      [&](const std::span<const AutomationEffect> effects) {
        return commit_automation_effects(session, effects, now,
                                         event.origin_token);
      });
  (void)poll_automation_cooldown_transitions(session, now, false);
  if (automation_runtime_host_.trace_subscribed() && live_edit_host_.authenticated() &&
      !session.automation_runtime->trace().empty()) {
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
        auto omitted = trace.front();
        omitted.phase = "error";
        omitted.detail = "source trace entry omitted because it exceeded the "
                         "live frame limit";
        omitted.source_event.reset();
        message.text = layerc::core::export_layerc_trace(
            std::span<const layerc::core::AutomationTraceEntry>(&omitted, 1U));
        if (!live_edit_host_.channel().send(message))
          break;
        session.automation_runtime->consume_trace(1U);
        continue;
      }
      message.text = layerc::core::export_layerc_trace(
          std::span<const layerc::core::AutomationTraceEntry>(trace.data(),
                                                              selected));
      if (!live_edit_host_.channel().send(message))
        break;
      session.automation_runtime->consume_trace(selected);
    }
  }
  synchronize_automation_state(session, now);
  schedule_automation_timer();
  return result.committed &&
         std::ranges::any_of(result.effects, [](const AutomationEffect &effect) {
           return layerc::core::effect_domain(effect.kind) !=
                  layerc::core::AutomationEffectDomain::internal;
         });
}

void DaemonApp::acknowledge_automation_state(const ProfileSource &source) {
  for (auto &session : session_registry_.sessions()) {
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
  const auto source = std::ranges::find(session_registry_.sources(), session.source_key,
                                        &ProfileSource::key);
  if (source == session_registry_.sources().end())
    return;
  source->automation_state =
      session.automation_runtime->snapshot().persistent_values;
  // Shared 1 s debounce (core::automation_checkpoint_arm): the first
  // mutation in a burst arms the deadline; later mutations keep it instead
  // of postponing the checkpoint forever under continuous input.
  layerc::core::automation_checkpoint_arm(source->automation_state_due, now);
  for (auto &other : session_registry_.sessions()) {
    if (other.get() == &session || !other->automation_runtime ||
        other->source_key != session.source_key)
      continue;
    other->automation_runtime->apply_persistent_values(
        source->automation_state);
  }
}

} // namespace layerc::daemon
