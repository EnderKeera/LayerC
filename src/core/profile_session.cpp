#include "layerc/core/profile_session.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace layerc::core {
namespace {
std::atomic_uint64_t origin_sequence{1};

void append(SessionFeedResult &to, SessionFeedResult from) {
  to.changed = from.changed || to.changed;
  to.matched = from.matched || to.matched;
  for (auto &id : from.triggered) to.triggered.push_back(std::move(id));
}
} // namespace

AutomationSession::AutomationSession(const GameProfile &profile)
    : cooldowns(profile.cooldowns, profile.cooldown_groups) {
  snapshot_buffer.reserve(profile.cooldowns.size());
}

std::uint64_t AutomationSession::next_origin() noexcept {
  return origin_sequence.fetch_add(1, std::memory_order_relaxed);
}

AutomationEvent AutomationSession::event(const AutomationEventKind kind,
                                         std::string source, const TimePoint now,
                                         const std::optional<std::uint64_t> origin) {
  return {.kind = kind, .timestamp = now, .source_id = std::move(source),
          .origin_token = origin.value_or(next_origin()), .fields = {}};
}

bool AutomationSession::start(AutomationProfile profile,
                              AutomationPersistentValues values,
                              const GameProfile &base, const TimePoint now,
                              AutomationSessionSink *sink,
                              const bool trace_enabled) {
  automation_runtime = std::make_unique<AutomationRuntime>(
      std::move(profile), std::move(values), base.cooldown_groups);
  automation_runtime->set_trace_enabled(trace_enabled);
  (void)poll_ready(base, now, false, sink);
  return process(event(AutomationEventKind::session_started, {}, now), base, sink);
}

bool AutomationSession::stop(const GameProfile &base, const TimePoint now,
                             AutomationSessionSink *sink) {
  if (!automation_runtime) return false;
  const bool changed = process(event(AutomationEventKind::session_stopping, {}, now), base, sink);
  if (sink != nullptr) sink->on_stopping(now);
  automation_runtime.reset();
  return changed;
}

bool AutomationSession::process(const AutomationEvent &value,
                                const GameProfile &base,
                                AutomationSessionSink *sink) {
  if (!automation_runtime) return false;
  const auto result = automation_runtime->process(
      value,
      [&](const AutomationEffect &effect) {
        return validate_automation_effect(effect, base.panels,
                                          base.cooldowns, base.cooldown_groups);
      },
      [&](const std::span<const AutomationEffect> effects) {
        return commit_automation_cooldown_effects(
            cooldowns, effects, base.cooldowns, base.cooldown_groups,
            value.timestamp, value.origin_token);
      });
  (void)poll_ready(base, value.timestamp, false, sink);
  if (sink != nullptr) {
    sink->on_result(result);
    if (automation_runtime->persistent_dirty())
      sink->on_persistent_change(
          automation_runtime->snapshot().persistent_values, value.timestamp);
  }
  return result.committed &&
         std::ranges::any_of(result.effects, [](const AutomationEffect &effect) {
           return effect_domain(effect.kind) != AutomationEffectDomain::internal;
         });
}

bool AutomationSession::poll_ready(const GameProfile &base, const TimePoint now,
                                   const bool emit, AutomationSessionSink *sink) {
  if (!automation_runtime) return false;
  cooldowns.snapshot(now, snapshot_buffer);
  const auto ready = poll_automation_ready_transitions(
      automation_runtime->edges(), snapshot_buffer, base.cooldown_groups);
  bool changed{};
  if (emit) {
    for (const auto &id : ready.cooldowns)
      changed = process(event(AutomationEventKind::cooldown_ready, id, now), base, sink) || changed;
    for (const auto &id : ready.groups)
      changed = process(event(AutomationEventKind::group_ready, id, now), base, sink) || changed;
  }
  return changed;
}

SessionFeedResult AutomationSession::trigger(
    const std::string_view id, const GameProfile &base, const TimePoint now,
    AutomationSessionSink *sink) {
  SessionFeedResult result;
  if (blocks_all_triggers()) return result;
  const auto origin = next_origin();
  result.triggered = cooldowns.trigger_with_propagation(id, now, origin);
  result.changed = !result.triggered.empty();
  for (const auto &triggered : result.triggered) {
    if (visual_triggers) visual_triggers->on_cooldown_triggered(triggered);
    if (automation_runtime)
      result.changed = process(event(AutomationEventKind::cooldown_triggered,
                                     triggered, now, origin), base, sink) || result.changed;
  }
  return result;
}

SessionFeedResult AutomationSession::trigger_requests(
    const std::span<const VisualTriggerRequest> requests,
    const GameProfile &base, const TimePoint now, AutomationSessionSink *sink) {
  SessionFeedResult result;
  for (const auto &request : requests)
    append(result, trigger(request.cooldown_id, base, now, sink));
  return result;
}

SessionFeedResult AutomationSession::feed_input(
    InputMatcher &matcher, const InputEvent &input, const GameProfile &base,
    const TimePoint now, AutomationSessionSink *sink) {
  SessionFeedResult result;
  if (automation_runtime) {
    result.changed = process(event(input.pressed ? AutomationEventKind::input_pressed
                                                  : AutomationEventKind::input_released,
                                   input_source_id(input.token), now), base, sink);
  }
  for (const auto &id : matcher.process(input)) {
    result.matched = true;
    if (blocks_all_triggers()) continue;
    if (visual_triggers) {
      append(result, trigger_requests(visual_triggers->on_input(id, now), base, now, sink));
    } else {
      const VisualTriggerRequest request{id, VisualTriggerSource::direct_input};
      append(result, trigger_requests(std::span{&request, 1U}, base, now, sink));
    }
  }
  return result;
}

std::optional<AutomationEvent>
AutomationSession::input_value_event(std::string source, double value,
                                     const TimePoint now, const bool momentary) {
  if (!automation_runtime || !std::isfinite(value)) return std::nullopt;
  const auto &profile = automation_runtime->profile();
  const bool demanded =
      std::ranges::any_of(profile.automations, [&](const auto &automation) {
        return automation.enabled &&
               std::ranges::any_of(automation.rules, [&](const auto &rule) {
                 return rule.event == AutomationEventKind::input_value_changed &&
                        (rule.source_filter.empty() ||
                         rule.source_filter == source);
               });
      });
  if (!demanded) return std::nullopt;

  const auto &settings = profile.continuous_input;
  if (std::abs(value) < settings.deadzone) value = 0.0;
  const auto minimum_interval = Milliseconds(std::max<std::uint32_t>(
      1U, 1'000U / std::max<std::uint32_t>(1U, settings.max_events_per_second)));
  auto &edges = automation_runtime->edges();
  const auto last = edges.input_emitted.find(source);
  if (last != edges.input_emitted.end() && now - last->second < minimum_interval)
    return std::nullopt;

  double previous_value = 0.0;
  if (!momentary) {
    const auto previous = edges.input_values.find(source);
    if (previous == edges.input_values.end()) {
      edges.input_values.emplace(std::move(source), value);
      return std::nullopt;
    }
    if (std::abs(value - previous->second) < settings.change_threshold)
      return std::nullopt;
    previous_value = previous->second;
    previous->second = value;
  }
  edges.input_emitted[source] = now;
  auto changed =
      event(AutomationEventKind::input_value_changed, std::move(source), now);
  changed.fields.emplace("previous", AutomationValue(previous_value));
  changed.fields.emplace("value", AutomationValue(value));
  return changed;
}

SessionFeedResult AutomationSession::feed_measurements(
    const std::span<const ObservationMeasurement> measurements,
    const GameProfile &base, const TimePoint now, AutomationSessionSink *sink) {
  if (!visual_triggers || blocks_all_triggers()) return {};
  cooldowns.snapshot(now, snapshot_buffer);
  SessionFeedResult result;
  if (automation_runtime) {
    for (const auto &measurement : measurements) {
      auto observation = event(AutomationEventKind::visual_observation,
                               measurement.observation_id, now);
      observation.fields.emplace("available", AutomationValue(
          measurement.availability == CaptureAvailability::available));
      observation.fields.emplace("matched", AutomationValue(measurement.matched));
      observation.fields.emplace("confidence", AutomationValue(
          ConfidenceValue{measurement.confidence}));
      result.changed = process(observation, base, sink) || result.changed;
    }
  }
  append(result, trigger_requests(visual_triggers->on_measurements(
      measurements, snapshot_buffer, now), base, now, sink));
  return result;
}

bool AutomationSession::poll(const GameProfile &base, const TimePoint now,
                             AutomationSessionSink *sink) {
  if (!automation_runtime) return false;
  bool changed = poll_ready(base, now, true, sink);
  changed = automation_runtime->expire_overlay_overrides(now) || changed;
  for (const auto &value : automation_runtime->due_timer_events(now))
    changed = process(value, base, sink) || changed;
  return changed;
}

bool AutomationSession::sync_persistent_to(AutomationSession &sibling) const {
  if (!automation_runtime || !automation_runtime->persistent_dirty() ||
      !sibling.automation_runtime) return false;
  sibling.automation_runtime->apply_persistent_values(
      automation_runtime->snapshot().persistent_values);
  return true;
}

std::optional<TimePoint> AutomationSession::next_deadline(
    const TimePoint now, const std::optional<TimePoint> checkpoint) {
  if (!automation_runtime) return checkpoint;
  cooldowns.snapshot(now, snapshot_buffer);
  return next_automation_deadline(*automation_runtime, snapshot_buffer, checkpoint);
}

} // namespace layerc::core
