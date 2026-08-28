#include "layerc/core/pixel_engine.hpp"

#include <algorithm>

namespace layerc::core {
namespace {

const CooldownState *find_cooldown(
    const std::span<const CooldownState> cooldowns, const std::string_view id) {
  const auto found = std::ranges::find_if(
      cooldowns, [&](const CooldownState &state) { return state.id == id; });
  return found == cooldowns.end() ? nullptr : &*found;
}

bool fully_ready(const CooldownState *state) {
  return state != nullptr && state->available_charges == state->max_charges &&
         !state->next_charge_at.has_value();
}

} // namespace

VisualTriggerEngine::VisualTriggerEngine(const VisualProfile &profile) {
  reconfigure(profile);
}

void VisualTriggerEngine::reconfigure(const VisualProfile &profile) {
  profile_ = profile;
  rules_.clear();
  triggers_.clear();
  rules_.reserve(profile_.rules.size() + profile_.templates.size());
  triggers_.reserve(profile_.triggers.size());
  for (const auto &rule : profile_.rules) rules_.try_emplace(rule.id);
  for (const auto &state : profile_.templates) rules_.try_emplace(state.id);
  for (const auto &trigger : profile_.triggers)
    triggers_.try_emplace(trigger.cooldown_id);
}

void VisualTriggerEngine::set_visual_enabled(const bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  clear_transient_state();
}

void VisualTriggerEngine::clear_transient_state() {
  for (auto &[id, state] : rules_) {
    (void)id;
    state = {};
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
    if (trigger->unavailable_policy ==
            CaptureUnavailablePolicy::trigger_from_input &&
        (rule == rules_.end() || !rule->second.has_sample ||
         rule->second.availability != CaptureAvailability::available))
      return {{trigger->cooldown_id,
               VisualTriggerSource::capture_fallback}};
    return {};
  }

  auto &trigger_state = triggers_.at(trigger->cooldown_id);
  const auto rule = rules_.find(trigger->observation_id);
  if (rule != rules_.end() && measurement_is_fresh(rule->second, now) &&
      rule->second.availability == CaptureAvailability::available &&
      rule->second.stable_match) {
    trigger_state.pending_confirmation = false;
    return {{trigger->cooldown_id, VisualTriggerSource::input_confirmed}};
  }

  if (trigger->unavailable_policy ==
          CaptureUnavailablePolicy::trigger_from_input &&
      (rule == rules_.end() || !rule->second.has_sample ||
       rule->second.availability != CaptureAvailability::available)) {
    trigger_state.pending_confirmation = false;
    return {{trigger->cooldown_id, VisualTriggerSource::capture_fallback}};
  }

  if (rule != rules_.end() && rule->second.has_sample &&
      is_terminal_unavailable(rule->second.availability)) {
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

std::vector<VisualTriggerRequest> VisualTriggerEngine::on_measurements(
    const std::span<const ObservationMeasurement> measurements,
    const std::span<const CooldownState> cooldowns, const TimePoint now) {
  std::vector<VisualTriggerRequest> output;
  if (!enabled_) return output;

  for (const auto &measurement : measurements) {
    const auto found = rules_.find(measurement.observation_id);
    if (!observation_exists(measurement.observation_id) ||
        found == rules_.end())
      continue;
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
      continue;
    }
    if (measurement.matched) {
      state.nonmatching_samples = 0;
      if (state.matching_samples < stable_samples(measurement.observation_id))
        ++state.matching_samples;
      if (state.matching_samples >=
          stable_samples(measurement.observation_id))
        state.stable_match = true;
    } else {
      state.matching_samples = 0;
      if (state.nonmatching_samples <
          stable_samples(measurement.observation_id))
        ++state.nonmatching_samples;
      if (state.nonmatching_samples >=
          stable_samples(measurement.observation_id))
        state.stable_match = false;
    }
  }

  for (const auto &trigger : profile_.triggers) {
    if (!trigger.enabled) continue;
    auto &trigger_state = triggers_.at(trigger.cooldown_id);
    const auto rule_it = rules_.find(trigger.observation_id);
    if (rule_it == rules_.end() ||
        !observation_exists(trigger.observation_id))
      continue;
    const auto &rule_state = rule_it->second;

    if (trigger.mode == VisualTriggerMode::confirm_input) {
      if (!trigger_state.pending_confirmation) continue;
      if (now > trigger_state.confirmation_deadline) {
        trigger_state.pending_confirmation = false;
        continue;
      }
      if (rule_state.has_sample &&
          rule_state.availability != CaptureAvailability::available &&
          trigger.unavailable_policy ==
              CaptureUnavailablePolicy::trigger_from_input) {
        trigger_state.pending_confirmation = false;
        output.push_back(
            {trigger.cooldown_id, VisualTriggerSource::capture_fallback});
      } else if (rule_state.has_sample &&
                 is_terminal_unavailable(rule_state.availability)) {
        trigger_state.pending_confirmation = false;
      } else if (measurement_is_fresh(rule_state, now) &&
                 rule_state.availability == CaptureAvailability::available &&
                 rule_state.stable_match) {
        trigger_state.pending_confirmation = false;
        output.push_back(
            {trigger.cooldown_id, VisualTriggerSource::input_confirmed});
      }
      continue;
    }

    if (rule_state.availability != CaptureAvailability::available ||
        !measurement_is_fresh(rule_state, now))
      continue;
    if (!rule_state.stable_match) {
      trigger_state.edge_armed = true;
      trigger_state.repeat_fired_for_ready = false;
      continue;
    }

    if (observation_repeat_policy(profile_, trigger.observation_id) ==
        PixelRepeatPolicy::edge) {
      if (trigger_state.edge_armed) {
        output.push_back(
            {trigger.cooldown_id, VisualTriggerSource::pixel_edge});
        trigger_state.edge_armed = false;
      }
      continue;
    }

    const auto ready =
        fully_ready(find_cooldown(cooldowns, trigger.cooldown_id));
    if (!ready) {
      trigger_state.repeat_fired_for_ready = false;
    } else if (!trigger_state.repeat_fired_for_ready) {
      output.push_back(
          {trigger.cooldown_id, VisualTriggerSource::pixel_repeat});
      trigger_state.repeat_fired_for_ready = true;
    }
  }
  return output;
}

bool VisualTriggerEngine::observation_exists(
    const std::string_view id) const {
  return has_observation(profile_, id);
}

std::uint8_t VisualTriggerEngine::stable_samples(
    const std::string_view id) const {
  return observation_stable_samples(profile_, id);
}

const VisualTrigger *
VisualTriggerEngine::find_trigger(const std::string_view cooldown_id) const {
  const auto found = std::ranges::find_if(
      profile_.triggers, [&](const VisualTrigger &trigger) {
        return trigger.cooldown_id == cooldown_id;
      });
  return found == profile_.triggers.end() ? nullptr : &*found;
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
