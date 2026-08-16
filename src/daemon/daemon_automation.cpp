#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
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

bool valid_overlay_effect(const AutomationEffect &effect) {
  const auto *spec =
      layerc::core::find_overlay_property(effect.property);
  if (spec == nullptr ||
      !layerc::core::overlay_value_valid(*spec, effect.value))
    return false;
  if (std::holds_alternative<std::nullptr_t>(effect.secondary.storage))
    return true;
  return layerc::core::as_duration(effect.secondary).has_value();
}

} // namespace

layerc::core::AutomationEvent DaemonApp::automation_event(
    const layerc::core::AutomationEventKind kind, std::string source_id,
    const layerc::core::TimePoint now) {
  return automation_event(kind, std::move(source_id), now,
                          automation_origin_sequence_++);
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
  for (auto &session : sessions_) {
    if (!session->has_enabled_automations()) {
      session->automation_runtime.reset();
      continue;
    }
    const auto source = std::ranges::find(profile_sources_, session->source_key,
                                          &ProfileSource::key);
    if (source == profile_sources_.end() ||
        !source->automation_profile.has_value()) {
      session->automation_runtime.reset();
      continue;
    }
    if (!session->automation_runtime) {
      session->automation_runtime =
          std::make_unique<layerc::core::AutomationRuntime>(
              *source->automation_profile, source->automation_state,
              session->profile.cooldown_groups);
      session->automation_runtime->set_trace_enabled(
          automation_trace_subscribed_);
      (void)poll_automation_cooldown_transitions(*session, now, false);
      overlays_changed =
          process_automation_event(
              *session,
              automation_event(AutomationEventKind::session_started, {}, now)) ||
          overlays_changed;
    } else if (session->automation_runtime->profile() !=
                   *source->automation_profile ||
               !std::ranges::equal(
                   session->automation_runtime->cooldown_groups(),
                   session->profile.cooldown_groups)) {
      session->automation_runtime->reconfigure(
          *source->automation_profile, session->profile.cooldown_groups);
      session->automation_runtime->apply_persistent_values(
          source->automation_state);
    }
  }
  const bool active = std::ranges::any_of(sessions_, [](const auto &session) {
    return session != nullptr && session->automation_runtime != nullptr;
  });
  if (active && automation_active_event_ == nullptr)
    automation_active_event_ = CreateEventW(
        nullptr, TRUE, FALSE, layerc::platform::automation_active_event_name);
  if (automation_active_event_ != nullptr) {
    if (active)
      SetEvent(automation_active_event_);
    else {
      CloseHandle(automation_active_event_);
      automation_active_event_ = nullptr;
    }
  }
  schedule_automation_timer();
  if (overlays_changed)
    update_overlays();
}

void DaemonApp::quiesce_automation_runtime() noexcept {
  if (automation_timer_running_ && window_ != nullptr)
    KillTimer(window_, automation_timer);
  automation_timer_running_ = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : sessions_) {
    if (!session->automation_runtime)
      continue;
    (void)process_automation_event(
        *session,
        automation_event(AutomationEventKind::session_stopping, {}, now));
    synchronize_automation_state(*session, now);
    session->automation_runtime.reset();
  }
  for (auto &source : profile_sources_) {
    if (!source.automation_state_due.has_value() ||
        !source.automation_profile.has_value())
      continue;
    if (layerc::platform::write_automation_state(
            source.path, *source.automation_profile, source.automation_state))
      source.automation_state_due.reset();
  }
  if (automation_active_event_ != nullptr) {
    CloseHandle(automation_active_event_);
    automation_active_event_ = nullptr;
  }
}

void DaemonApp::retire_automation_session(Session &session,
                                          const TimePoint now) {
  if (!session.automation_runtime)
    return;
  (void)process_automation_event(
      session,
      automation_event(AutomationEventKind::session_stopping, {}, now));
  synchronize_automation_state(session, now);
  const auto source = std::ranges::find(profile_sources_, session.source_key,
                                        &ProfileSource::key);
  if (source != profile_sources_.end() &&
      source->automation_state_due.has_value() &&
      source->automation_profile.has_value() &&
      layerc::platform::write_automation_state(
          source->path, *source->automation_profile, source->automation_state))
    source->automation_state_due.reset();
  session.automation_runtime.reset();
  session.applied_overlay_overrides.clear();
}
void DaemonApp::schedule_automation_timer() {
  if (window_ == nullptr)
    return;
  const auto now = layerc::core::MonotonicClock::now();
  std::optional<TimePoint> next;
  for (auto &session : sessions_) {
    if (!session->automation_runtime)
      continue;
    const auto due = session->automation_runtime->next_timer_due();
    if (due.has_value() && (!next.has_value() || *due < *next))
      next = due;
    session->cooldowns.snapshot(now, session->snapshot_buffer);
    for (const auto &state : session->snapshot_buffer) {
      if (state.next_charge_at.has_value() &&
          (!next.has_value() || *state.next_charge_at < *next))
        next = state.next_charge_at;
    }
  }
  for (const auto &source : profile_sources_) {
    if (source.automation_state_due.has_value() &&
        (!next.has_value() || *source.automation_state_due < *next))
      next = source.automation_state_due;
  }
  if (automation_timer_running_) {
    KillTimer(window_, automation_timer);
    automation_timer_running_ = false;
  }
  if (!next.has_value())
    return;
  const auto remaining =
      std::max(Milliseconds(1), std::chrono::ceil<Milliseconds>(*next - now));
  const auto delay = static_cast<UINT>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<UINT>::max()));
  automation_timer_running_ =
      SetTimer(window_, automation_timer, delay, nullptr) != 0;
}
void DaemonApp::poll_automation_timer() {
  if (automation_timer_running_)
    KillTimer(window_, automation_timer);
  automation_timer_running_ = false;
  const auto now = layerc::core::MonotonicClock::now();
  bool changed{};
  for (auto &session : sessions_) {
    if (!session->automation_runtime)
      continue;
    changed =
        poll_automation_cooldown_transitions(*session, now, true) || changed;
    const auto overrides_before =
        session->automation_runtime->snapshot().overlay_overrides;
    const auto events = session->automation_runtime->due_timer_events(now);
    changed = changed ||
              overrides_before !=
                  session->automation_runtime->snapshot().overlay_overrides;
    for (const auto &event : events)
      changed = process_automation_event(*session, event) || changed;
    synchronize_automation_state(*session, now);
  }
  for (auto &source : profile_sources_) {
    if (!source.automation_state_due.has_value() ||
        *source.automation_state_due > now ||
        !source.automation_profile.has_value())
      continue;
    if (layerc::platform::write_automation_state(
            source.path, *source.automation_profile, source.automation_state)) {
      source.automation_state_due.reset();
      acknowledge_automation_state(source);
    } else
      source.automation_state_due = now + Milliseconds(1'000);
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
  auto &edges = session.automation_runtime->edges();
  session.cooldowns.snapshot(now, session.snapshot_buffer);
  std::map<std::string, bool, std::less<>> cooldown_ready;
  bool changed{};
  for (const auto &state : session.snapshot_buffer) {
    const bool ready = state.available_charges == state.max_charges;
    cooldown_ready[state.id] = ready;
    const auto previous = edges.cooldown_ready.find(state.id);
    if (emit && previous != edges.cooldown_ready.end() &&
        !previous->second && ready) {
      changed = process_automation_event(
                    session,
                    automation_event(AutomationEventKind::cooldown_ready,
                                     state.id, now)) ||
                changed;
    }
  }
  edges.cooldown_ready = std::move(cooldown_ready);

  std::map<std::string, bool, std::less<>> group_ready;
  for (const auto &group : session.profile.cooldown_groups) {
    const bool ready =
        !group.cooldown_ids.empty() &&
        std::ranges::all_of(group.cooldown_ids, [&](const std::string &id) {
          const auto found = edges.cooldown_ready.find(id);
          return found != edges.cooldown_ready.end() && found->second;
        });
    group_ready[group.id] = ready;
    const auto previous = edges.group_ready.find(group.id);
    if (emit && previous != edges.group_ready.end() &&
        !previous->second && ready) {
      changed = process_automation_event(
                    session,
                    automation_event(AutomationEventKind::group_ready,
                                     group.id, now)) ||
                changed;
    }
  }
  edges.group_ready = std::move(group_ready);
  return changed;
}
bool DaemonApp::validate_automation_effect(
    const Session &session, const AutomationEffect &effect) const {
  switch (layerc::core::effect_domain(effect.kind)) {
  case layerc::core::AutomationEffectDomain::overlay:
    if (effect.kind != AutomationActionKind::set_overlay_override)
      return true;
    return valid_overlay_effect(effect) &&
           std::ranges::any_of(session.profile.panels, [&](const auto &panel) {
             return panel.id == effect.target;
           });
  case layerc::core::AutomationEffectDomain::cooldown_engine:
    if (effect.kind == AutomationActionKind::reduce_cooldown_next ||
        effect.kind == AutomationActionKind::reduce_cooldown_all) {
      if (!layerc::core::as_duration(effect.value).has_value())
        return false;
      return std::ranges::any_of(
          session.profile.cooldowns, [&](const auto &cooldown) {
            return cooldown.id == effect.target;
          });
    }
    if (effect.kind == AutomationActionKind::trigger_group ||
        effect.kind == AutomationActionKind::reset_group)
      return std::ranges::any_of(
          session.profile.cooldown_groups,
          [&](const auto &group) { return group.id == effect.target; });
    return std::ranges::any_of(
        session.profile.cooldowns,
        [&](const auto &cooldown) { return cooldown.id == effect.target; });
  case layerc::core::AutomationEffectDomain::internal:
    return true;
  }
  return true;
}

bool DaemonApp::commit_automation_effects(
    Session &session, const std::span<const AutomationEffect> effects,
    const TimePoint now, const std::uint64_t origin_token) {
  auto staged = session.cooldowns;
  for (const auto &effect : effects) {
    if (layerc::core::effect_domain(effect.kind) ==
        layerc::core::AutomationEffectDomain::cooldown_engine) {
      if (!layerc::core::apply_automation_cooldown_effect(staged, effect, now,
                                                          origin_token))
        return false;
    }
  }
  session.cooldowns = std::move(staged);
  return true;
}
bool DaemonApp::trigger_cooldown_with_automation(
    Session &session, const std::string_view cooldown_id, const TimePoint now) {
  const auto origin = automation_origin_sequence_++;
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
  if (automation_trace_subscribed_ && live_package_authenticated_ &&
      !session.automation_runtime->trace().empty()) {
    layerc::core::LiveMessage message;
    message.type = layerc::core::LiveMessageType::automation_trace_event;
    message.target_window =
        reinterpret_cast<std::uintptr_t>(session.game_window);
    message.profile_key = layerc::platform::wide_to_utf8(session.source_key);
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
        if (!live_channel_.send(message))
          break;
        session.automation_runtime->consume_trace(1U);
        continue;
      }
      message.text = layerc::core::export_layerc_trace(
          std::span<const layerc::core::AutomationTraceEntry>(trace.data(),
                                                              selected));
      if (!live_channel_.send(message))
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
  for (auto &session : sessions_) {
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
  const auto source = std::ranges::find(profile_sources_, session.source_key,
                                        &ProfileSource::key);
  if (source == profile_sources_.end())
    return;
  source->automation_state =
      session.automation_runtime->snapshot().persistent_values;
  // Debounce the first mutation in a burst, but do not postpone the durable
  // checkpoint forever while a repeating timer or continuous input keeps
  // changing persistent values.
  if (!source->automation_state_due.has_value())
    source->automation_state_due = now + Milliseconds(1'000);
  for (auto &other : sessions_) {
    if (other.get() == &session || !other->automation_runtime ||
        other->source_key != session.source_key)
      continue;
    other->automation_runtime->apply_persistent_values(
        source->automation_state);
  }
}

} // namespace layerc::daemon