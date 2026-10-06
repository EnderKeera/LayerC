#include "layerc/core/visual_trigger_engine.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <algorithm>

namespace layerc::core {
namespace {

const CooldownState *find_cooldown(
    const std::span<const CooldownState> cooldowns, const std::string_view id) {
  const auto found = std::ranges::find(cooldowns, id, &CooldownState::id);
  return found == cooldowns.end() ? nullptr : &*found;
}

bool fully_ready(const CooldownState *state) {
  return state != nullptr && cooldown_ready(*state);
}

} // namespace

VisualTriggerEngine::VisualTriggerEngine(const VisualProfile &profile) {
  reconfigure(profile);
}

void VisualTriggerEngine::reconfigure(const VisualProfile &profile) {
  profile_ = profile;
  rules_.clear();
  triggers_.clear();
  trigger_indices_.clear();
  rules_.reserve(profile_.observations.size());
  triggers_.reserve(profile_.triggers.size());
  trigger_indices_.reserve(profile_.triggers.size());
  for (const auto &observation : profile_.observations)
    std::visit(
        [&](const auto &item) {
          rules_.try_emplace(item.id,
                             RuleState{.stable_samples = item.stable_samples});
        },
        observation);
  for (std::size_t index{}; index < profile_.triggers.size(); ++index) {
    const auto &trigger = profile_.triggers[index];
    triggers_.try_emplace(trigger.cooldown_id);
    trigger_indices_.try_emplace(trigger.cooldown_id, index);
  }
}

void VisualTriggerEngine::set_visual_enabled(const bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  clear_transient_state();
}

void VisualTriggerEngine::clear_transient_state() {
  for (auto &[id, state] : rules_) {
    (void)id;
    const auto stable_samples = state.stable_samples;
    state = RuleState{.stable_samples = stable_samples};
  }
  for (auto &[id, state] : triggers_) {
    (void)id;
    state = {};
  }
}

void VisualTriggerEngine::on_cooldown_triggered(
    const std::string_view cooldown_id) {
  const auto found = triggers_.find(std::string(cooldown_id));
  if (found != triggers_.end())
    found->second.repeat_fired_for_ready = false;
}

std::vector<VisualTriggerRequest>
VisualTriggerEngine::on_input(const std::string_view cooldown_id,
                              const TimePoint now) {
  const auto *trigger = find_trigger(cooldown_id);
  if (!enabled_ || trigger == nullptr || !trigger->enabled)
    return {{std::string(cooldown_id), VisualTriggerSource::direct_input}};

  if (trigger->mode == VisualTriggerMode::pixel_only) {
    if (trigger->keep_input)
      return {{std::string(cooldown_id), VisualTriggerSource::direct_input}};
    const auto rule = rules_.find(trigger->observation_id);
    if (classify(*trigger, rule == rules_.end() ? nullptr : &rule->second,
                 now) == ConfirmationDecision::fallback)
      return {{trigger->cooldown_id,
               VisualTriggerSource::capture_fallback}};
    return {};
  }

  auto &trigger_state = triggers_.at(trigger->cooldown_id);
  const auto rule = rules_.find(trigger->observation_id);
  const auto decision = classify(
      *trigger, rule == rules_.end() ? nullptr : &rule->second, now);
  if (decision == ConfirmationDecision::confirmed) {
    trigger_state.pending_confirmation = false;
    return {{trigger->cooldown_id, VisualTriggerSource::input_confirmed}};
  }

  if (decision == ConfirmationDecision::fallback) {
    trigger_state.pending_confirmation = false;
    return {{trigger->cooldown_id, VisualTriggerSource::capture_fallback}};
  }

  if (decision == ConfirmationDecision::terminal) {
    trigger_state.pending_confirmation = false;
    return {};
  }

  // Every physical input edge deliberately starts a fresh, bounded
  // confirmation attempt. Repeated presses are separate attempts rather than
  // an extension performed by the capture stream itself.
  trigger_state.pending_confirmation = true;
  trigger_state.confirmation_deadline = now + trigger->confirmation_timeout;
  return {};
}

void VisualTriggerEngine::apply_measurement(
    const ObservationMeasurement &measurement) {
  const auto found = rules_.find(measurement.observation_id);
  if (found == rules_.end()) return;
  auto &state = found->second;
  state.availability = measurement.availability;
  state.sampled_at = measurement.sampled_at;
  state.has_sample = true;
  if (measurement.availability != CaptureAvailability::available) {
    state.matching_samples = 0;
    state.nonmatching_samples = 0;
    state.stable_match = false;
    for (const auto &trigger : profile_.triggers) {
      if (trigger.observation_id != measurement.observation_id) continue;
      auto &trigger_state = triggers_.at(trigger.cooldown_id);
      trigger_state.edge_armed = true;
      trigger_state.repeat_fired_for_ready = false;
    }
    return;
  }
  if (measurement.matched) {
    state.nonmatching_samples = 0;
    if (state.matching_samples < state.stable_samples)
      ++state.matching_samples;
    if (state.matching_samples >= state.stable_samples)
      state.stable_match = true;
  } else {
    state.matching_samples = 0;
    if (state.nonmatching_samples < state.stable_samples)
      ++state.nonmatching_samples;
    if (state.nonmatching_samples >= state.stable_samples)
      state.stable_match = false;
  }
}

void VisualTriggerEngine::evaluate_confirm_trigger(
    const VisualTrigger &trigger, TriggerState &trigger_state,
    const RuleState &rule_state, const TimePoint now,
    std::vector<VisualTriggerRequest> &output) {
  if (!trigger_state.pending_confirmation) return;
  if (now > trigger_state.confirmation_deadline) {
    trigger_state.pending_confirmation = false;
    return;
  }
  const auto decision = classify(trigger, &rule_state, now);
  if (decision == ConfirmationDecision::fallback) {
    trigger_state.pending_confirmation = false;
    output.push_back({trigger.cooldown_id, VisualTriggerSource::capture_fallback});
  } else if (decision == ConfirmationDecision::terminal) {
    trigger_state.pending_confirmation = false;
  } else if (decision == ConfirmationDecision::confirmed) {
    trigger_state.pending_confirmation = false;
    output.push_back({trigger.cooldown_id, VisualTriggerSource::input_confirmed});
  }
}

void VisualTriggerEngine::evaluate_pixel_trigger(
    const VisualTrigger &trigger, TriggerState &trigger_state,
    const RuleState &rule_state, const std::span<const CooldownState> cooldowns,
    const TimePoint now, std::vector<VisualTriggerRequest> &output) {
  if (rule_state.availability != CaptureAvailability::available ||
      !measurement_is_fresh(rule_state, now))
    return;
  if (!rule_state.stable_match) {
    trigger_state.edge_armed = true;
    trigger_state.repeat_fired_for_ready = false;
    return;
  }

  const auto *observation = find_observation(profile_, trigger.observation_id);
  if (observation == nullptr ||
      observation_repeat(*observation) == PixelRepeatPolicy::edge) {
    if (trigger_state.edge_armed) {
      output.push_back({trigger.cooldown_id, VisualTriggerSource::pixel_edge});
      trigger_state.edge_armed = false;
    }
    return;
  }

  const auto ready =
      fully_ready(find_cooldown(cooldowns, trigger.cooldown_id));
  if (!ready) {
    trigger_state.repeat_fired_for_ready = false;
  } else if (!trigger_state.repeat_fired_for_ready) {
    output.push_back({trigger.cooldown_id, VisualTriggerSource::pixel_repeat});
    trigger_state.repeat_fired_for_ready = true;
  }
}

std::vector<VisualTriggerRequest> VisualTriggerEngine::on_measurements(
    const std::span<const ObservationMeasurement> measurements,
    const std::span<const CooldownState> cooldowns, const TimePoint now) {
  std::vector<VisualTriggerRequest> output;
  if (!enabled_) return output;

  for (const auto &measurement : measurements) apply_measurement(measurement);

  for (const auto &trigger : profile_.triggers) {
    if (!trigger.enabled) continue;
    auto &trigger_state = triggers_.at(trigger.cooldown_id);
    const auto rule_it = rules_.find(trigger.observation_id);
    if (rule_it == rules_.end()) continue;
    const auto &rule_state = rule_it->second;

    if (trigger.mode == VisualTriggerMode::confirm_input) {
      evaluate_confirm_trigger(trigger, trigger_state, rule_state, now, output);
      continue;
    }
    evaluate_pixel_trigger(trigger, trigger_state, rule_state, cooldowns, now,
                           output);
  }
  return output;
}

const VisualTrigger *
VisualTriggerEngine::find_trigger(const std::string_view cooldown_id) const {
  const auto found = trigger_indices_.find(std::string(cooldown_id));
  return found == trigger_indices_.end() ? nullptr
                                         : &profile_.triggers[found->second];
}

VisualTriggerEngine::ConfirmationDecision VisualTriggerEngine::classify(
    const VisualTrigger &trigger, const RuleState *state,
    const TimePoint now) const {
  if (state == nullptr || !state->has_sample) {
    return trigger.unavailable_policy ==
                   CaptureUnavailablePolicy::trigger_from_input
               ? ConfirmationDecision::fallback
               : ConfirmationDecision::wait;
  }
  if (state->availability != CaptureAvailability::available) {
    if (trigger.unavailable_policy ==
        CaptureUnavailablePolicy::trigger_from_input)
      return ConfirmationDecision::fallback;
    return is_terminal_unavailable(state->availability)
               ? ConfirmationDecision::terminal
               : ConfirmationDecision::wait;
  }
  return measurement_is_fresh(*state, now) && state->stable_match
             ? ConfirmationDecision::confirmed
             : ConfirmationDecision::wait;
}

bool VisualTriggerEngine::measurement_is_fresh(const RuleState &state,
                                               const TimePoint now) const {
  if (!state.has_sample || state.sampled_at > now) return false;
  // Three configured sample intervals tolerate worker/scheduler jitter while
  // confirmation_timeout still bounds each user-input attempt.
  const auto maximum_age = Milliseconds(3'000 / profile_.sample_rate_hz);
  return now - state.sampled_at <= maximum_age;
}

bool VisualTriggerEngine::is_terminal_unavailable(
    const CaptureAvailability availability) noexcept {
  return availability == CaptureAvailability::unavailable ||
         availability == CaptureAvailability::protected_or_black ||
         availability == CaptureAvailability::outside_output;
}

} // namespace layerc::core
