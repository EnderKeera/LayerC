#include "automation_runtime_impl.hpp"

#include "automation_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace layerc::core {
namespace {
using detail::checked_integer_binary;
using detail::duration_result;
using detail::number;
} // namespace

std::string runtime_effect_key(const AutomationEffect &effect) {
  switch (effect.kind) {
  case AutomationActionKind::set_variable:
  case AutomationActionKind::increment_variable:
  case AutomationActionKind::append_list:
  case AutomationActionKind::set_map_entry:
  case AutomationActionKind::erase_map_entry:
    return "v:" + effect.target;
  case AutomationActionKind::trigger_cooldown:
  case AutomationActionKind::reset_cooldown:
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
  case AutomationActionKind::refund_cooldown:
    return "c:" + effect.target;
  case AutomationActionKind::trigger_group:
  case AutomationActionKind::reset_group:
    return "g:" + effect.target;
  case AutomationActionKind::create_timer:
  case AutomationActionKind::cancel_timer:
    return "t:" + effect.target;
  case AutomationActionKind::set_overlay_override:
    // Names and property names never contain ':'; the item ID is last.
    return "o:" + effect.name + ":" + effect.property + ":" + effect.item_id;
  case AutomationActionKind::clear_overlay_override:
    return "o:" + effect.target + ":*";
  }
  return effect.target;
}

bool runtime_effect_precedes(const AutomationEffect &left,
                             const AutomationEffect &right) {
  return effect_order_precedes(left.order, right.order);
}

bool AutomationRuntime::Impl::apply_internal(
    const AutomationEffect &effect, TimePoint now,
    AutomationRuntimeSnapshot &target, AutomationError &error,
    std::vector<AutomationEvent> &derived) const {
  if (auto invalid = automation_effect_error(effect)) {
    error = std::move(*invalid);
    return false;
  }
  auto variable = target.variables.find(effect.target);
  const auto finish_mutation = [&](AutomationValue before) {
    // Whole-value validity is the postcondition, including collection keys
    // and nesting. Restore the attributable mutation before deriving events.
    if (!automation_value_valid(variable->second)) {
      variable->second = std::move(before);
      error = {"$runtime", "variable mutation exceeds automation bounds"};
      return false;
    }
    if (before == variable->second)
      return true;
    AutomationEvent event;
    event.kind = AutomationEventKind::variable_changed;
    event.timestamp = now;
    event.source_id = effect.target;
    event.fields.emplace(automation_event_field::old_value, std::move(before));
    event.fields.emplace(automation_event_field::value, variable->second);
    derived.push_back(std::move(event));
    return true;
  };
  const auto emit = [&](const AutomationEventKind kind,
                        const std::string &source_id) {
    derived.push_back({.kind = kind,
                       .timestamp = now,
                       .source_id = source_id,
                       .origin_token = 0,
                       .fields = {}});
  };
  switch (effect.kind) {
  case AutomationActionKind::set_variable:
    if (variable == target.variables.end()) {
      error = {"$runtime", "invalid variable assignment"};
      return false;
    }
    if (value_type(variable->second) != value_type(effect.value)) {
      error = {"$runtime", "variable assignment changes its declared type"};
      return false;
    }
    {
      auto before = variable->second;
      variable->second = effect.value;
      return finish_mutation(std::move(before));
    }
  case AutomationActionKind::increment_variable: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    const auto before = variable->second;
    if (const auto *integer_value =
            std::get_if<std::int64_t>(&before.storage)) {
      const auto *right = std::get_if<std::int64_t>(&effect.value.storage);
      const auto sum =
          right == nullptr
              ? std::optional<std::int64_t>{}
              : checked_integer_binary(*integer_value, *right,
                                       detail::AutomationOpcode::add);
      if (!sum.has_value()) {
        error = {"$runtime", "integer increment overflow"};
        return false;
      }
      variable->second = AutomationValue(*sum);
    } else if (const auto *number_value =
                   std::get_if<double>(&before.storage)) {
      const auto right = number(effect.value);
      if (!right.has_value() || !std::isfinite(*number_value + *right)) {
        error = {"$runtime", "number increment is not finite"};
        return false;
      }
      variable->second = AutomationValue(*number_value + *right);
    } else if (const auto *duration_value =
                   std::get_if<DurationValue>(&before.storage)) {
      const auto *right = std::get_if<DurationValue>(&effect.value.storage);
      const auto sum =
          right == nullptr
              ? std::optional<AutomationValue>{}
              : duration_result(
                    static_cast<double>(duration_value->value.count()) +
                    static_cast<double>(right->value.count()));
      if (!sum.has_value()) {
        error = {"$runtime", "duration increment is out of range"};
        return false;
      }
      variable->second = *sum;
    } else if (const auto *confidence_value =
                   std::get_if<ConfidenceValue>(&before.storage)) {
      const auto *right = std::get_if<ConfidenceValue>(&effect.value.storage);
      if (right == nullptr) {
        error = {"$runtime", "confidence increment is out of range"};
        return false;
      }
      const double sum = confidence_value->value + right->value;
      if (!std::isfinite(sum) || sum < 0.0 || sum > 1.0) {
        error = {"$runtime", "confidence increment is out of range"};
        return false;
      }
      variable->second = AutomationValue(ConfidenceValue{sum});
    } else {
      error = {"$runtime", "increment requires numeric values"};
      return false;
    }
    return finish_mutation(before);
  }
  case AutomationActionKind::append_list: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    auto *list = std::get_if<AutomationList>(&variable->second.storage);
    if (!list) {
      error = {"$runtime", "target is not a list"};
      return false;
    }
    auto before = variable->second;
    list->push_back(effect.value);
    return finish_mutation(std::move(before));
  }
  case AutomationActionKind::set_map_entry: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    auto *map = std::get_if<AutomationMap>(&variable->second.storage);
    const auto *key = std::get_if<std::string>(&effect.secondary.storage);
    if (!map || !key) {
      error = {"$runtime", "target is not a map or map key is invalid"};
      return false;
    }
    auto before = variable->second;
    (*map)[*key] = effect.value;
    return finish_mutation(std::move(before));
  }
  case AutomationActionKind::erase_map_entry: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    auto *map = std::get_if<AutomationMap>(&variable->second.storage);
    const auto *key = std::get_if<std::string>(&effect.value.storage);
    if (!map || !key) {
      error = {"$runtime", "eraseMapEntry requires a string key"};
      return false;
    }
    auto before = variable->second;
    map->erase(*key);
    return finish_mutation(std::move(before));
  }
  case AutomationActionKind::create_timer: {
    const auto interval = as_duration(effect.value);
    const auto due = automation_time_after(now, *interval);
    if (!due) {
      error = {"$runtime", "timer deadline is outside the clock range"};
      return false;
    }
    auto found =
        std::ranges::find(target.timers, effect.target, &AutomationTimer::id);
    if (found == target.timers.end()) {
      if (target.timers.size() >= max_automation_timers) {
        error = {"$runtime", "session timer limit reached"};
        return false;
      }
      target.timers.push_back({effect.target, effect.automation_id, *due,
                               *interval, effect.repeating});
    } else
      *found = {effect.target, effect.automation_id, *due, *interval,
                effect.repeating};
    return true;
  }
  case AutomationActionKind::cancel_timer:
    std::erase_if(target.timers,
                  [&](const auto &timer) { return timer.id == effect.target; });
    return true;
  case AutomationActionKind::set_overlay_override: {
    std::optional<TimePoint> expiry;
    if (!std::holds_alternative<std::nullptr_t>(effect.secondary.storage)) {
      const auto duration = as_duration(effect.secondary);
      if (*duration > Milliseconds::zero()) {
        expiry = automation_time_after(now, *duration);
        if (!expiry) {
          error = {"$runtime", "overlay expiry is outside the clock range"};
          return false;
        }
      }
    }
    std::erase_if(target.overlay_overrides, [&](const auto &item) {
      return item.name == effect.name && item.item_id == effect.item_id &&
             item.property == effect.property;
    });
    target.overlay_overrides.push_back(
        {.name = effect.name,
         .owner_automation = effect.automation_id,
         .panel_id = effect.target,
         .item_id = effect.item_id,
         .property = effect.property,
         .value = effect.value,
         .expires_at = expiry,
         .order = effect.order});
    return true;
  }
  case AutomationActionKind::clear_overlay_override:
    std::erase_if(target.overlay_overrides,
                  [&](const auto &item) { return item.name == effect.target; });
    return true;
  case AutomationActionKind::trigger_cooldown: {
    // Closure semantics (deliberate): the runtime does not own the
    // CooldownEngine, so intra-batch cooldown_triggered events expand the
    // trigger-propagation closure from the profile's cooldown groups.
    // Whether a charge is actually consumed is decided by the daemon-side
    // committer against the real engine; the daemon's own trigger paths
    // emit engine-derived events with real origin tokens. A rule keyed on
    // cooldown_triggered inside the triggering batch therefore observes
    // "the trigger action was issued", not "a charge was consumed".
    const std::array<std::string, 1> seed{effect.target};
    for (const auto &id : groups.trigger_closure(seed))
      emit(AutomationEventKind::cooldown_triggered, id);
    return true;
  }
  case AutomationActionKind::reset_cooldown:
    for (const auto &id : groups.reset_members(effect.target))
      emit(AutomationEventKind::cooldown_ready, id);
    return true;
  case AutomationActionKind::trigger_group:
    emit(AutomationEventKind::group_triggered, effect.target);
    for (const auto &id : groups.group_trigger_members(effect.target))
      emit(AutomationEventKind::cooldown_triggered, id);
    return true;
  case AutomationActionKind::reset_group:
    emit(AutomationEventKind::group_ready, effect.target);
    for (const auto &id : groups.group_reset_members(effect.target))
      emit(AutomationEventKind::cooldown_ready, id);
    return true;
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
    return true;
  default:
    return true;
  }
}

AttemptOutcome
AutomationRuntime::Impl::commit_batch(EvaluationStage &stage,
                                      const EffectValidator &validator,
                                      const EffectCommitter &committer) {
  for (const auto &winner : stage.winners) {
    if (auto exhausted = stage.budget.check())
      return *exhausted;
    const auto &effect = winner.second;
    if (validator && !validator(effect))
      return AutomationFault{
          effect.automation_id,
          {"$runtime", "transaction validation rejected an action"}};
    stage.result.effects.push_back(effect);
  }
  std::ranges::sort(stage.result.effects, runtime_effect_precedes);
  AutomationRuntimeSnapshot committed_state = stage.root_snapshot;
  for (const auto &effect : stage.result.effects) {
    if (auto exhausted = stage.budget.check())
      return *exhausted;
    std::vector<AutomationEvent> ignored;
    AutomationError apply_error;
    if (!apply_internal(effect, effect.timestamp, committed_state, apply_error,
                        ignored))
      return AutomationFault{effect.automation_id, std::move(apply_error)};
  }
  // The complete winning set is valid before crossing the external boundary.
  // Callbacks are synchronous and cannot be forcibly preempted.
  if (auto exhausted = stage.budget.check())
    return *exhausted;
  if (committer && !committer(stage.result.effects))
    return AttemptRejected{
        {"$runtime", "transaction commit rejected the action batch"}};
  // Dirtiness is the commit's diff over the persistent variables.
  for (const auto &variable : profile.variables) {
    if (!variable.persistent)
      continue;
    const auto before = state.variables.find(variable.id);
    const auto after = committed_state.variables.find(variable.id);
    const bool had = before != state.variables.end();
    const bool has = after != committed_state.variables.end();
    if (had != has || (had && before->second != after->second))
      persistent_dirty = true;
  }
  state = std::move(committed_state);
  // queue[0] is the root event; only later entries are derived.
  stage.result.derived_events.assign(stage.queue.begin() + 1,
                                     stage.queue.end());
  stage.result.committed = true;
  return AttemptComplete{};
}
} // namespace layerc::core
