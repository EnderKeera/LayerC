#include "automation_runtime_impl.hpp"

#include "automation_ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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
    return "o:" + effect.name + ":" + effect.property;
  case AutomationActionKind::clear_overlay_override:
    return "o:" + effect.target + ":*";
  }
  return effect.target;
}

bool runtime_effect_precedes(const AutomationEffect &left,
                             const AutomationEffect &right) {
  if (left.priority != right.priority)
    return left.priority > right.priority;
  if (left.automation_order != right.automation_order)
    return left.automation_order < right.automation_order;
  return left.rule_order < right.rule_order;
}

std::vector<std::string> AutomationRuntime::Impl::propagated_cooldowns(
    const std::span<const std::string> seeds) const {
  std::vector<std::string> result(seeds.begin(), seeds.end());
  std::set<std::string, std::less<>> seen(result.begin(), result.end());
  for (std::size_t index{}; index < result.size(); ++index) {
    for (const auto &group : cooldown_groups) {
      if (group.mode != CooldownGroupMode::trigger_propagation ||
          std::ranges::find(group.cooldown_ids, result[index]) ==
              group.cooldown_ids.end())
        continue;
      for (const auto &member : group.cooldown_ids)
        if (seen.insert(member).second)
          result.push_back(member);
    }
  }
  return result;
}

std::vector<std::string>
AutomationRuntime::Impl::group_cooldowns(const std::string_view group_id,
                                         const bool propagate) const {
  const auto group =
      std::ranges::find(cooldown_groups, group_id, &CooldownGroup::id);
  if (group == cooldown_groups.end())
    return {};
  return propagate ? propagated_cooldowns(group->cooldown_ids)
                   : group->cooldown_ids;
}

std::vector<std::string> AutomationRuntime::Impl::reset_cooldowns(
    const std::string_view cooldown_id) const {
  for (const auto &group : cooldown_groups)
    if (group.mode == CooldownGroupMode::shared_pool &&
        std::ranges::find(group.cooldown_ids, cooldown_id) !=
            group.cooldown_ids.end())
      return group.cooldown_ids;
  return {std::string(cooldown_id)};
}

bool AutomationRuntime::Impl::apply_internal(
    const AutomationEffect &effect, TimePoint now,
    AutomationRuntimeSnapshot &target, AutomationError &error,
    std::vector<AutomationEvent> &derived) const {
  auto variable = target.variables.find(effect.target);
  const auto changed = [&](const AutomationValue &before,
                           const AutomationValue &after) {
    if (before == after)
      return;
    AutomationEvent event;
    event.kind = AutomationEventKind::variable_changed;
    event.timestamp = now;
    event.source_id = effect.target;
    event.fields.emplace("old", before);
    event.fields.emplace("value", after);
    derived.push_back(std::move(event));
  };
  switch (effect.kind) {
  case AutomationActionKind::set_variable:
    if (variable == target.variables.end() ||
        !automation_value_valid(effect.value)) {
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
      changed(before, variable->second);
    }
    return true;
  case AutomationActionKind::increment_variable: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    const auto before = variable->second;
    if (const auto *integer_value =
            std::get_if<std::int64_t>(&before.storage)) {
      const auto *right = std::get_if<std::int64_t>(&effect.value.storage);
      const auto sum = right == nullptr ? std::optional<std::int64_t>{}
                                        : checked_integer_binary(*integer_value,
                                                                 *right, "add");
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
      const double sum =
          right == nullptr ? -1.0 : confidence_value->value + right->value;
      if (!std::isfinite(sum) || sum < 0.0 || sum > 1.0) {
        error = {"$runtime", "confidence increment is out of range"};
        return false;
      }
      variable->second = AutomationValue(ConfidenceValue{sum});
    } else {
      error = {"$runtime", "increment requires numeric values"};
      return false;
    }
    changed(before, variable->second);
    return true;
  }
  case AutomationActionKind::append_list: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    auto *list = std::get_if<AutomationList>(&variable->second.storage);
    if (!list || list->size() >= max_automation_collection_entries) {
      error = {"$runtime", "list is full or target is not a list"};
      return false;
    }
    auto before = variable->second;
    list->push_back(effect.value);
    changed(before, variable->second);
    return true;
  }
  case AutomationActionKind::set_map_entry: {
    if (variable == target.variables.end()) {
      error = {"$runtime", "unknown variable"};
      return false;
    }
    auto *map = std::get_if<AutomationMap>(&variable->second.storage);
    const auto *key = std::get_if<std::string>(&effect.secondary.storage);
    if (!map || !key ||
        (map->size() >= max_automation_collection_entries &&
         !map->contains(*key))) {
      error = {"$runtime", "map is full or map key is invalid"};
      return false;
    }
    auto before = variable->second;
    (*map)[*key] = effect.value;
    changed(before, variable->second);
    return true;
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
    changed(before, variable->second);
    return true;
  }
  case AutomationActionKind::create_timer: {
    const auto interval = as_duration(effect.value);
    if (!interval || *interval < min_automation_timer_interval) {
      error = {"$runtime", "timer interval must be at least 50 ms"};
      return false;
    }
    auto found =
        std::ranges::find(target.timers, effect.target, &AutomationTimer::id);
    if (found == target.timers.end()) {
      if (target.timers.size() >= max_automation_timers) {
        error = {"$runtime", "session timer limit reached"};
        return false;
      }
      target.timers.push_back({effect.target, effect.automation_id,
                               now + *interval, *interval, effect.repeating});
    } else
      *found = {effect.target, effect.automation_id, now + *interval, *interval,
                effect.repeating};
    return true;
  }
  case AutomationActionKind::cancel_timer:
    std::erase_if(target.timers,
                  [&](const auto &timer) { return timer.id == effect.target; });
    return true;
  case AutomationActionKind::set_overlay_override: {
    std::optional<TimePoint> expiry;
    if (const auto value = as_duration(effect.secondary);
        value && *value > Milliseconds::zero())
      expiry = now + *value;
    std::erase_if(target.overlay_overrides, [&](const auto &item) {
      return item.name == effect.name && item.property == effect.property;
    });
    target.overlay_overrides.push_back(
        {.name = effect.name,
         .owner_automation = effect.automation_id,
         .panel_id = effect.target,
         .property = effect.property,
         .value = effect.value,
         .expires_at = expiry,
         .priority = effect.priority,
         .automation_order = effect.automation_order,
         .rule_order = effect.rule_order});
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
    for (const auto &id : propagated_cooldowns(seed))
      derived.push_back({.kind = AutomationEventKind::cooldown_triggered,
                         .timestamp = now,
                         .source_id = id,
                         .origin_token = 0,
                         .fields = {}});
    return true;
  }
  case AutomationActionKind::reset_cooldown:
    for (const auto &id : reset_cooldowns(effect.target))
      derived.push_back({.kind = AutomationEventKind::cooldown_ready,
                         .timestamp = now,
                         .source_id = id,
                         .origin_token = 0,
                         .fields = {}});
    return true;
  case AutomationActionKind::trigger_group:
    derived.push_back({.kind = AutomationEventKind::group_triggered,
                       .timestamp = now,
                       .source_id = effect.target,
                       .origin_token = 0,
                       .fields = {}});
    for (const auto &id : group_cooldowns(effect.target, true))
      derived.push_back({.kind = AutomationEventKind::cooldown_triggered,
                         .timestamp = now,
                         .source_id = id,
                         .origin_token = 0,
                         .fields = {}});
    return true;
  case AutomationActionKind::reset_group:
    derived.push_back({.kind = AutomationEventKind::group_ready,
                       .timestamp = now,
                       .source_id = effect.target,
                       .origin_token = 0,
                       .fields = {}});
    for (const auto &id : group_cooldowns(effect.target, false))
      derived.push_back({.kind = AutomationEventKind::cooldown_ready,
                         .timestamp = now,
                         .source_id = id,
                         .origin_token = 0,
                         .fields = {}});
    return true;
  default:
    return true;
  }
}

std::optional<std::string> AutomationRuntime::Impl::commit_batch(
    const AutomationRuntimeSnapshot &root_snapshot,
    const std::vector<AutomationEvent> &queue,
    const std::map<std::string, AutomationEffect, std::less<>> &winners,
    const EffectValidator &validator, const EffectCommitter &committer,
    AutomationEvaluationResult &result, AutomationError &fault) {
  for (const auto &[key, effect] : winners) {
    (void)key;
    if (faulted.contains(effect.automation_id))
      continue;
    if (validator && !validator(effect)) {
      const AutomationError rejection{
          "$runtime", "transaction validation rejected an action"};
      fault = rejection;
      return effect.automation_id;
    }
    result.effects.push_back(effect);
  }
  std::ranges::sort(result.effects, runtime_effect_precedes);
  AutomationRuntimeSnapshot committed_state = root_snapshot;
  for (const auto &effect : result.effects) {
    std::vector<AutomationEvent> ignored;
    AutomationError apply_error;
    if (!apply_internal(effect, effect.timestamp, committed_state, apply_error,
                        ignored)) {
      fault = apply_error;
      return effect.automation_id;
    }
  }
  if (committer && !committer(result.effects)) {
    result.error = AutomationError{
        "$runtime", "transaction commit rejected the action batch"};
    return std::nullopt;
  }
  state = std::move(committed_state);
  for (const auto &variable : profile.variables)
    if (variable.persistent) {
      const auto found = state.variables.find(variable.id);
      if (found == state.variables.end())
        continue;
      auto &saved = state.persistent_values[variable.id];
      if (saved != found->second) {
        saved = found->second;
        persistent_dirty = true;
      }
    }
  result.derived_events.assign(queue.begin() + 1, queue.end());
  result.committed = true;
  return std::nullopt;
}
} // namespace layerc::core
