#include "layerc/core/automation_session.hpp"

#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <ranges>
#include <utility>

namespace layerc::core {
namespace {
bool validate_automation_cooldown_effect(
    const AutomationEffect &effect,
    const std::span<const CooldownDefinition> cooldowns_,
    const std::span<const CooldownGroup> groups) {
  if (effect_domain(effect.kind) != AutomationEffectDomain::cooldown_engine)
    return false;
  if (effect.kind == AutomationActionKind::trigger_group ||
      effect.kind == AutomationActionKind::reset_group)
    return std::ranges::any_of(groups, [&](const CooldownGroup &group) {
      return group.id == effect.target;
    });
  return std::ranges::any_of(
      cooldowns_, [&](const CooldownDefinition &cooldown) {
        return cooldown.id == effect.target;
      });
}
} // namespace

bool validate_session_effect(
    const AutomationEffect &effect, const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> cooldowns_,
    const std::span<const CooldownGroup> groups) {
  if (automation_effect_error(effect)) return false;
  switch (effect_domain(effect.kind)) {
  case AutomationEffectDomain::overlay:
    if (effect.kind != AutomationActionKind::set_overlay_override) return true;
    return std::ranges::any_of(panels, [&](const Panel &panel) {
             return panel.id == effect.target &&
                    (effect.item_id.empty() ||
                     panel_item_targetable(panel, effect.item_id));
           });
  case AutomationEffectDomain::cooldown_engine:
    return validate_automation_cooldown_effect(effect, cooldowns_, groups);
  case AutomationEffectDomain::internal:
    return true;
  default:
    return false;
  }
}

namespace {
bool apply_validated_cooldown_effect(CooldownEngine &cooldowns,
                                      const AutomationEffect &effect,
                                      const TimePoint now,
                                      const std::uint64_t origin_token) {
  switch (effect.kind) {
  case AutomationActionKind::trigger_cooldown:
    (void)cooldowns.trigger_with_propagation(effect.target, now, origin_token);
    return true;
  case AutomationActionKind::reset_cooldown:
    (void)cooldowns.reset(effect.target);
    return true;
  case AutomationActionKind::refund_cooldown:
    (void)cooldowns.refund(effect.target, now);
    return true;
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all: {
    const auto duration = as_duration(effect.value);
    (void)cooldowns.reduce(
        effect.target, *duration,
        effect.kind == AutomationActionKind::reduce_cooldown_all, now);
    return true;
  }
  case AutomationActionKind::trigger_group:
    (void)cooldowns.trigger_group(effect.target, now, origin_token);
    return true;
  case AutomationActionKind::reset_group:
    (void)cooldowns.reset_group(effect.target);
    return true;
  default:
    return false;
  }
}
} // namespace

bool commit_automation_cooldown_effects(
    CooldownEngine &engine, const std::span<const AutomationEffect> effects,
    const TimePoint now,
    const std::uint64_t origin_token) {
  // The complete winning set passed session admission before this callback.
  // Stage the engine so an application failure cannot publish a partial batch.
  auto staged = engine;
  for (const auto &effect : effects) {
    if (effect_domain(effect.kind) !=
        AutomationEffectDomain::cooldown_engine)
      continue;
    if (!apply_validated_cooldown_effect(staged, effect, now, origin_token))
      return false;
  }
  engine = std::move(staged);
  return true;
}

namespace {
std::optional<TimePoint> next_automation_deadline(
    const AutomationRuntime &runtime, const std::span<const CooldownState> cooldowns_,
    const std::optional<TimePoint> checkpoint) {
  auto next = checkpoint;
  const auto include = [&](const std::optional<TimePoint> due) {
    if (due && (!next || *due < *next)) next = due;
  };
  include(runtime.next_deadline());
  for (const auto &state : cooldowns_)
    include(state.next_charge_at);
  return next;
}

struct ReadyTransitions {
  std::vector<std::string> cooldowns_;
  std::vector<std::string> groups;
};

ReadyTransitions poll_automation_ready_transitions(
    AutomationSessionEdges &edges,
    const std::span<const CooldownState> snapshot,
    const std::span<const CooldownGroup> groups) {
  ReadyTransitions ready;
  std::map<std::string, bool, std::less<>> ready_by_id;
  for (const auto &state : snapshot) {
    const bool is_ready = cooldown_ready(state);
    ready_by_id[state.id] = is_ready;
    const auto previous = edges.cooldown_ready.find(state.id);
    if (previous != edges.cooldown_ready.end() && !previous->second &&
        is_ready)
      ready.cooldowns_.push_back(state.id);
  }
  // Groups read from this poll's local map (assigned to edges only at the
  // end): one snapshot in, one consistent verdict out. The maps are sized
  // by the snapshot (bounded by the profile's cooldown/group counts), so
  // the per-poll allocation stays proportional on the 2 ms budget.
  std::map<std::string, bool, std::less<>> group_ready;
  for (const auto &group : groups) {
    const bool is_ready =
        !group.cooldown_ids.empty() &&
        std::ranges::all_of(group.cooldown_ids, [&](const std::string &id) {
          const auto found = ready_by_id.find(id);
          return found != ready_by_id.end() && found->second;
        });
    group_ready[group.id] = is_ready;
    const auto previous = edges.group_ready.find(group.id);
    if (previous != edges.group_ready.end() && !previous->second && is_ready)
      ready.groups.push_back(group.id);
  }
  edges.cooldown_ready = std::move(ready_by_id);
  edges.group_ready = std::move(group_ready);
  return ready;
}


std::atomic_uint64_t origin_sequence{1};

void append(SessionFeedResult &to, SessionFeedResult from) {
  to.changed = from.changed || to.changed;
  to.matched = from.matched || to.matched;
  for (auto &id : from.triggered) to.triggered.push_back(std::move(id));
}
} // namespace

void AutomationRuntimeHealth::record(const AutomationEvaluationResult &result) {
  if (result.faulted_automations.empty()) {
    if (result.error.has_value())
      runtime_error = result.error->describe();
    else
      runtime_error.reset();
    return;
  }
  runtime_error.reset();
  for (const auto &id : result.faulted_automations)
    runtime_faults.insert_or_assign(id, "faulted during event");
}

std::vector<std::string>
AutomationRuntimeHealth::lines(const std::string_view source) const {
  std::vector<std::string> result;
  const std::string prefix(source);
  if (runtime_error) result.push_back(prefix + ": " + *runtime_error);
  for (const auto &[id, error] : runtime_faults)
    result.push_back(prefix + ": automation fault: " + id + ": " + error);
  return result;
}

AutomationSession::AutomationSession(GameProfile matched)
    : profile_(std::move(matched)),
      cooldowns_(profile_.cooldowns, profile_.cooldown_groups) {
  snapshot_buffer_.reserve(profile_.cooldowns.size());
}

void AutomationSession::reconfigure_profile(
    GameProfile matched, const std::span<const std::string> reset_cooldown_ids,
    const TimePoint now) {
  cooldowns_.reconfigure(matched.cooldowns, reset_cooldown_ids, now,
                        matched.cooldown_groups);
  profile_ = std::move(matched);
  snapshot_buffer_.clear();
  snapshot_buffer_.reserve(profile_.cooldowns.size());
}

void AutomationSession::configure_visual(
    const std::optional<VisualProfile> &visual, const bool enabled) {
  if (!visual) {
    visual_triggers_.reset();
    return;
  }
  if (visual_triggers_)
    visual_triggers_->reconfigure(*visual);
  else
    visual_triggers_ = std::make_unique<VisualTriggerEngine>(*visual);
  visual_triggers_->set_visual_enabled(enabled);
}

std::span<const CooldownState> AutomationSession::snapshot(const TimePoint now) {
  cooldowns_.snapshot(now, snapshot_buffer_);
  return snapshot_buffer_;
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

bool AutomationSession::start(AutomationProfile automation,
                              AutomationPersistentValues values,
                              const TimePoint now, AutomationSessionSink *sink,
                              const bool trace_enabled, AutomationRuntime::ClockReader clock) {
  if (!clock) clock = std::chrono::steady_clock::now;
  automation_runtime_ = std::make_unique<AutomationRuntime>(
      std::move(automation), std::move(values), cooldowns_.group_topology(),
      std::move(clock));
  automation_runtime_->set_trace_enabled(trace_enabled);
  (void)poll_ready(now, false, sink);
  return process(event(AutomationEventKind::session_started, {}, now), sink);
}

bool AutomationSession::stop(const TimePoint now, AutomationSessionSink *sink) {
  if (!automation_runtime_) return false;
  const bool changed = process(event(AutomationEventKind::session_stopping, {}, now), sink);
  if (sink != nullptr) sink->on_stopping(now);
  automation_runtime_.reset();
  return changed;
}

bool AutomationSession::process(const AutomationEvent &value,
                                AutomationSessionSink *sink) {
  if (!automation_runtime_) return false;
  const auto result = automation_runtime_->process(
      value,
      [&](const AutomationEffect &effect) {
        // An active window that cannot be armed rejects the whole winning set
        // before any external effect commits.
        if ((effect.kind == AutomationActionKind::trigger_cooldown ||
             effect.kind == AutomationActionKind::trigger_group) &&
            !cooldowns_.can_arm_active(value.timestamp))
          return false;
        return validate_session_effect(effect, profile_.panels,
                                          profile_.cooldowns,
                                          profile_.cooldown_groups);
      },
      [&](const std::span<const AutomationEffect> effects) {
        return commit_automation_cooldown_effects(
            cooldowns_, effects, value.timestamp, value.origin_token);
      });
  (void)poll_ready(value.timestamp, false, sink);
  automation_health_.record(result);
  if (sink != nullptr) {
    sink->on_result(result);
    if (automation_runtime_->persistent_dirty())
      sink->on_persistent_change(
          automation_runtime_->persistent_values(), value.timestamp);
  }
  return result.committed &&
         std::ranges::any_of(result.effects, [](const AutomationEffect &effect) {
           return effect_domain(effect.kind) != AutomationEffectDomain::internal;
         });
}

bool AutomationSession::poll_ready(const TimePoint now, const bool emit,
                                   AutomationSessionSink *sink) {
  if (!automation_runtime_) return false;
  cooldowns_.snapshot(now, snapshot_buffer_);
  const auto ready = poll_automation_ready_transitions(
      automation_runtime_->edges(), snapshot_buffer_, profile_.cooldown_groups);
  bool changed{};
  if (emit) {
    for (const auto &id : ready.cooldowns_)
      changed = process(event(AutomationEventKind::cooldown_ready, id, now), sink) || changed;
    for (const auto &id : ready.groups)
      changed = process(event(AutomationEventKind::group_ready, id, now), sink) || changed;
  }
  return changed;
}

SessionFeedResult AutomationSession::trigger(
    const std::string_view id, const TimePoint now, AutomationSessionSink *sink) {
  SessionFeedResult result;
  if (blocks_all_triggers()) return result;
  const auto origin = next_origin();
  result.triggered = cooldowns_.trigger_with_propagation(id, now, origin);
  result.changed = !result.triggered.empty();
  for (const auto &triggered : result.triggered) {
    if (visual_triggers_) visual_triggers_->on_cooldown_triggered(triggered);
    if (automation_runtime_)
      result.changed = process(event(AutomationEventKind::cooldown_triggered,
                                     triggered, now, origin), sink) || result.changed;
  }
  return result;
}

SessionFeedResult AutomationSession::trigger_requests(
    const std::span<const VisualTriggerRequest> requests, const TimePoint now,
    AutomationSessionSink *sink) {
  SessionFeedResult result;
  for (const auto &request : requests)
    append(result, trigger(request.cooldown_id, now, sink));
  return result;
}

SessionFeedResult AutomationSession::feed_input(
    InputMatcher &matcher, const InputEvent &input, const TimePoint now,
    AutomationSessionSink *sink) {
  SessionFeedResult result;
  if (automation_runtime_) {
    result.changed = process(event(input.pressed ? AutomationEventKind::input_pressed
                                                  : AutomationEventKind::input_released,
                                   input_source_id(input.token), now), sink);
  }
  for (const auto &id : matcher.process(input)) {
    result.matched = true;
    if (blocks_all_triggers()) continue;
    if (visual_triggers_) {
      append(result, trigger_requests(visual_triggers_->on_input(id, now), now, sink));
    } else {
      const VisualTriggerRequest request{id, VisualTriggerSource::direct_input};
      append(result, trigger_requests(std::span{&request, 1U}, now, sink));
    }
  }
  return result;
}

std::optional<AutomationEvent>
AutomationSession::input_value_event(std::string source, double value,
                                     const TimePoint now, const bool momentary) {
  if (!automation_runtime_ || !std::isfinite(value)) return std::nullopt;
  const auto &automation_profile = automation_runtime_->profile();
  const bool demanded = std::ranges::any_of(
      automation_profile.automations, [&](const auto &automation) {
        return automation.enabled &&
               std::ranges::any_of(automation.rules, [&](const auto &rule) {
                 return rule.event == AutomationEventKind::input_value_changed &&
                        (rule.source_filter.empty() ||
                         rule.source_filter == source);
               });
      });
  if (!demanded) return std::nullopt;

  const auto &settings = automation_profile.continuous_input;
  if (std::abs(value) < settings.deadzone) value = 0.0;
  const auto minimum_interval = Milliseconds(std::max<std::uint32_t>(
      1U, 1'000U / std::max<std::uint32_t>(1U, settings.max_events_per_second)));
  auto &edges = automation_runtime_->edges();
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
  changed.fields.emplace(automation_event_field::previous,
                         AutomationValue(previous_value));
  changed.fields.emplace(automation_event_field::value, AutomationValue(value));
  return changed;
}

SessionFeedResult AutomationSession::feed_measurements(
    const std::span<const ObservationMeasurement> measurements,
    const TimePoint now, AutomationSessionSink *sink) {
  if (!visual_triggers_ || blocks_all_triggers()) return {};
  cooldowns_.snapshot(now, snapshot_buffer_);
  SessionFeedResult result;
  if (automation_runtime_) {
    for (const auto &measurement : measurements) {
      auto observation = event(AutomationEventKind::visual_observation,
                               measurement.observation_id, now);
      observation.fields.emplace(
          automation_event_field::available,
          AutomationValue(measurement.availability ==
                          CaptureAvailability::available));
      observation.fields.emplace(automation_event_field::matched,
                                 AutomationValue(measurement.matched));
      observation.fields.emplace(
          automation_event_field::confidence,
          AutomationValue(ConfidenceValue{measurement.confidence}));
      result.changed = process(observation, sink) || result.changed;
    }
  }
  append(result, trigger_requests(visual_triggers_->on_measurements(
      measurements, snapshot_buffer_, now), now, sink));
  return result;
}

bool AutomationSession::poll(const TimePoint now, AutomationSessionSink *sink) {
  if (!automation_runtime_) return false;
  bool changed = poll_ready(now, true, sink);
  changed = automation_runtime_->expire_overlay_overrides(now) || changed;
  for (const auto &value : automation_runtime_->due_timer_events(now))
    changed = process(value, sink) || changed;
  return changed;
}

bool AutomationSession::sync_persistent_to(AutomationSession &sibling) const {
  if (!automation_runtime_ || !automation_runtime_->persistent_dirty() ||
      !sibling.automation_runtime_) return false;
  sibling.automation_runtime_->apply_persistent_values(
      automation_runtime_->persistent_values());
  return true;
}

std::optional<TimePoint> AutomationSession::next_deadline(
    const TimePoint now, const std::optional<TimePoint> checkpoint) {
  if (!automation_runtime_) return checkpoint;
  cooldowns_.snapshot(now, snapshot_buffer_);
  return next_automation_deadline(*automation_runtime_, snapshot_buffer_, checkpoint);
}

} // namespace layerc::core
