#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/live_edit.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

namespace layerc::core {
namespace {
using detail::checked_integer_binary;
using detail::duration_result;
using detail::number;
using detail::truthy;
std::string effect_key(const AutomationEffect &effect) {
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
bool effect_precedes(const AutomationEffect &left,
                     const AutomationEffect &right) {
  if (left.priority != right.priority)
    return left.priority > right.priority;
  if (left.automation_order != right.automation_order)
    return left.automation_order < right.automation_order;
  return left.rule_order < right.rule_order;
}
} // namespace

struct AutomationRuntime::Impl {
  AutomationProfile profile;
  std::vector<CooldownGroup> cooldown_groups;
  AutomationRuntimeSnapshot state;
  AutomationSessionEdges edges;
  std::set<std::string, std::less<>> faulted;
  bool persistent_dirty{};
  bool trace_enabled{};
  std::uint64_t trace_sequence{};
  std::vector<AutomationTraceEntry> trace_entries;

  Impl(AutomationProfile source, AutomationPersistentValues persistent,
       const std::span<const CooldownGroup> groups)
      : profile(std::move(source)),
        cooldown_groups(groups.begin(), groups.end()) {
    for (const auto &variable : profile.variables) {
      auto value = variable.default_value;
      if (variable.persistent) {
        const auto found = persistent.find(variable.id);
        if (found != persistent.end() &&
            value_type(found->second) == variable.type &&
            automation_value_valid(found->second))
          value = found->second;
        state.persistent_values[variable.id] = value;
      }
      state.variables[variable.id] = std::move(value);
    }
  }
  void make_trace_room() {
    if (trace_entries.size() == 2048)
      trace_entries.erase(trace_entries.begin(), trace_entries.begin() + 256);
  }
  void add_source_trace(const AutomationEvent &event) {
    if (!trace_enabled)
      return;
    make_trace_room();
    AutomationTraceEntry entry;
    entry.sequence = ++trace_sequence;
    entry.timestamp = std::chrono::duration_cast<Milliseconds>(
        event.timestamp.time_since_epoch());
    entry.phase = "source";
    entry.detail = std::string(detail::automation_event_name(event.kind));
    entry.source_event = event;
    trace_entries.push_back(std::move(entry));
  }
  void add_trace(const AutomationEvent &event, std::string automation,
                 std::string rule, std::string phase, std::string detail) {
    if (!trace_enabled)
      return;
    make_trace_room();
    AutomationTraceEntry entry;
    entry.sequence = ++trace_sequence;
    entry.timestamp = std::chrono::duration_cast<Milliseconds>(
        event.timestamp.time_since_epoch());
    entry.automation_id = std::move(automation);
    entry.rule_id = std::move(rule);
    entry.phase = std::move(phase);
    entry.detail = std::move(detail);
    trace_entries.push_back(std::move(entry));
  }
  std::optional<AutomationValue> eval(const AutomationExpression &expression,
                                      const AutomationEvent &event,
                                      const AutomationRuntimeSnapshot &snapshot,
                                      std::size_t &evaluations,
                                      AutomationError &error) const {
    if (++evaluations > max_automation_node_evaluations) {
      error = {"$runtime", "root event exceeded 8192 node evaluations"};
      return std::nullopt;
    }
    switch (expression.kind) {
    case ExpressionKind::literal:
      return expression.literal;
    case ExpressionKind::variable: {
      const auto found = snapshot.variables.find(expression.reference);
      if (found == snapshot.variables.end()) {
        error = {"$runtime", "unknown variable " + expression.reference};
        return std::nullopt;
      }
      return found->second;
    }
    case ExpressionKind::event_field: {
      if (expression.reference == "ageMs") {
        const auto recorded = event.fields.find("ageMs");
        if (recorded != event.fields.end())
          return recorded->second;
        return AutomationValue(DurationValue{Milliseconds::zero()});
      }
      if (expression.reference == "sourceId")
        return AutomationValue(event.source_id);
      const auto found = event.fields.find(expression.reference);
      return found == event.fields.end() ? AutomationValue{} : found->second;
    }
    case ExpressionKind::index: {
      auto target =
          eval(expression.arguments[0], event, snapshot, evaluations, error);
      auto key =
          eval(expression.arguments[1], event, snapshot, evaluations, error);
      if (!target || !key)
        return std::nullopt;
      if (const auto *list = std::get_if<AutomationList>(&target->storage)) {
        const auto *index = std::get_if<std::int64_t>(&key->storage);
        if (!index || *index < 0 ||
            static_cast<std::size_t>(*index) >= list->size())
          return AutomationValue{};
        return (*list)[static_cast<std::size_t>(*index)];
      }
      if (const auto *map = std::get_if<AutomationMap>(&target->storage)) {
        const auto *name = std::get_if<std::string>(&key->storage);
        if (!name)
          return AutomationValue{};
        const auto found = map->find(*name);
        return found == map->end() ? AutomationValue{} : found->second;
      }
      error = {"$runtime", "index target is not a list or map"};
      return std::nullopt;
    }
    default:
      break;
    }
    std::vector<AutomationValue> args;
    args.reserve(expression.arguments.size());
    for (const auto &argument : expression.arguments) {
      auto value = eval(argument, event, snapshot, evaluations, error);
      if (!value)
        return std::nullopt;
      args.push_back(std::move(*value));
    }
    const auto *spec = detail::find_automation_op(expression.operation);
    if (spec == nullptr) {
      error = {"$runtime", "unsupported expression operation"};
      return std::nullopt;
    }
    AutomationValue evaluated;
    std::string message;
    if (!spec->evaluate(*spec, args, evaluated, message)) {
      error = {"$runtime", message};
      return std::nullopt;
    }
    return evaluated;
  }
  std::vector<std::string>
  propagated_cooldowns(const std::span<const std::string> seeds) const {
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

  std::vector<std::string> group_cooldowns(const std::string_view group_id,
                                           const bool propagate) const {
    const auto group =
        std::ranges::find(cooldown_groups, group_id, &CooldownGroup::id);
    if (group == cooldown_groups.end())
      return {};
    return propagate ? propagated_cooldowns(group->cooldown_ids)
                     : group->cooldown_ids;
  }

  std::vector<std::string>
  reset_cooldowns(const std::string_view cooldown_id) const {
    for (const auto &group : cooldown_groups)
      if (group.mode == CooldownGroupMode::shared_pool &&
          std::ranges::find(group.cooldown_ids, cooldown_id) !=
              group.cooldown_ids.end())
        return group.cooldown_ids;
    return {std::string(cooldown_id)};
  }

  bool apply_internal(const AutomationEffect &effect, TimePoint now,
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
        const auto sum =
            right == nullptr
                ? std::optional<std::int64_t>{}
                : checked_integer_binary(*integer_value, *right, "add");
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
        *found = {effect.target, effect.automation_id, now + *interval,
                  *interval, effect.repeating};
      return true;
    }
    case AutomationActionKind::cancel_timer:
      std::erase_if(target.timers, [&](const auto &timer) {
        return timer.id == effect.target;
      });
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
      std::erase_if(target.overlay_overrides, [&](const auto &item) {
        return item.name == effect.target;
      });
      return true;
    case AutomationActionKind::trigger_cooldown: {
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

  // One full evaluation attempt over the root event. Returns the automation
  // to fault and restart without when the attempt failed transactionally;
  // returns nullopt when result is final (committed, rejected, or capped).
  // node_evaluations and faulted_automations accumulate across attempts and
  // are owned by the caller.
  std::optional<std::string>
  evaluate(const AutomationEvent &root, const EffectValidator &validator,
           const EffectCommitter &committer, const TimePoint deadline,
           AutomationEvaluationResult &result, AutomationError &fault);
};
AutomationRuntime::AutomationRuntime(
    AutomationProfile profile, AutomationPersistentValues persistent,
    const std::span<const CooldownGroup> cooldown_groups)
    : impl_(std::make_unique<Impl>(std::move(profile), std::move(persistent),
                                   cooldown_groups)) {}
AutomationRuntime::~AutomationRuntime() = default;
const AutomationProfile &AutomationRuntime::profile() const noexcept {
  return impl_->profile;
}
std::span<const CooldownGroup>
AutomationRuntime::cooldown_groups() const noexcept {
  return impl_->cooldown_groups;
}
bool AutomationRuntime::active() const noexcept {
  return std::ranges::any_of(impl_->profile.automations, [&](const auto &a) {
    return a.enabled && !impl_->faulted.contains(a.id);
  });
}
const AutomationRuntimeSnapshot &AutomationRuntime::snapshot() const noexcept {
  return impl_->state;
}
AutomationSessionEdges &AutomationRuntime::edges() noexcept {
  return impl_->edges;
}
const AutomationSessionEdges &AutomationRuntime::edges() const noexcept {
  return impl_->edges;
}
AutomationEvaluationResult
AutomationRuntime::process(const AutomationEvent &root,
                           const EffectValidator &validator,
                           const EffectCommitter &committer) {
  AutomationEvaluationResult result;
  if (root.source_id.size() > max_automation_string_bytes ||
      !detail::is_valid_utf8(root.source_id) ||
      root.fields.size() > max_automation_collection_entries ||
      !std::ranges::all_of(root.fields, [](const auto &field) {
        return field.first.size() <= max_automation_string_bytes &&
               detail::is_valid_utf8(field.first) &&
               automation_value_valid(field.second);
      })) {
    result.error = {"$event", "source event exceeds automation bounds"};
    return result;
  }
  // One deadline spans the whole fault-retry cascade: restarting the
  // evaluation used to grant each recursive retry a fresh budget, so a
  // chain of faulted automations could block the daemon loop for up to
  // max_automation_rules x 2 ms on a single root event.
  const auto deadline = MonotonicClock::now() + automation_execution_budget;
  std::optional<AutomationError> last_fault;
  for (;;) {
    const auto trace_sequence_start = impl_->trace_sequence;
    AutomationError fault;
    const auto restart_with = impl_->evaluate(root, validator, committer,
                                              deadline, result, fault);
    if (!restart_with.has_value()) {
      if (!result.error.has_value() && last_fault.has_value())
        result.error = *last_fault;
      return result;
    }
    // Rewind this attempt's source-phase traces so the restart re-records
    // them; condition/error traces stay as the fault's audit trail.
    std::erase_if(impl_->trace_entries, [&](const auto &entry) {
      return entry.sequence > trace_sequence_start && entry.phase == "source";
    });
    impl_->faulted.insert(*restart_with);
    if (std::ranges::find(result.faulted_automations, *restart_with) ==
        result.faulted_automations.end())
      result.faulted_automations.push_back(*restart_with);
    last_fault = fault;
  }
}

std::optional<std::string> AutomationRuntime::Impl::evaluate(
    const AutomationEvent &root, const EffectValidator &validator,
    const EffectCommitter &committer, const TimePoint deadline,
    AutomationEvaluationResult &result, AutomationError &fault) {
  result.effects.clear();
  result.derived_events.clear();
  result.committed = false;
  result.error.reset();
  const auto root_snapshot = state;
  auto staged = root_snapshot;
  std::vector<AutomationEvent> queue{root};
  // Rule identity as (automation order << 32) | rule order: the match loop
  // already tracks both indices, so dedup needs no per-rule key allocation.
  const auto fired_key = [](const std::size_t ai, const std::size_t ri) {
    return (static_cast<std::uint64_t>(ai) << 32U) |
           static_cast<std::uint64_t>(ri);
  };
  std::set<std::uint64_t> fired;
  std::map<std::string, AutomationEffect, std::less<>> winners;
  std::size_t queue_index{};
  while (queue_index < queue.size()) {
    if (queue_index >= max_automation_derived_events + 1U) {
      // The transaction cannot complete within the derived-event cap: fail
      // the whole batch without committing partial effects.
      result.error =
          AutomationError{"$runtime", "root event exceeded 32 derived events"};
      return std::nullopt;
    }
    const auto event = queue[queue_index++];
    add_source_trace(event);
    struct Match {
      const AutomationDefinition *automation;
      const AutomationRule *rule;
      std::size_t ai;
      std::size_t ri;
    };
    std::vector<Match> matches;
    for (std::size_t ai{}; ai < profile.automations.size(); ++ai) {
      const auto &automation = profile.automations[ai];
      if (!automation.enabled || faulted.contains(automation.id))
        continue;
      for (std::size_t ri{}; ri < automation.rules.size(); ++ri) {
        const auto &rule = automation.rules[ri];
        if (rule.event == event.kind &&
            (rule.source_filter.empty() ||
             rule.source_filter == event.source_id) &&
            !fired.contains(fired_key(ai, ri)))
          matches.push_back({&automation, &rule, ai, ri});
      }
    }
    std::ranges::stable_sort(matches, [](const Match &a, const Match &b) {
      if (a.rule->priority != b.rule->priority)
        return a.rule->priority > b.rule->priority;
      if (a.ai != b.ai)
        return a.ai < b.ai;
      return a.ri < b.ri;
    });
    for (const auto &match : matches) {
      if (MonotonicClock::now() > deadline) {
        const AutomationError timeout{"$runtime",
                                      "automation execution exceeded 2 ms"};
        add_trace(event, match.automation->id, match.rule->id, "error",
                  timeout.message);
        fault = timeout;
        return match.automation->id;
      }
      AutomationError error;
      auto condition = eval(match.rule->condition, event, root_snapshot,
                            result.node_evaluations, error);
      if (!condition) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      if (!truthy(*condition)) {
        add_trace(event, match.automation->id, match.rule->id, "condition",
                  "false");
        continue;
      }
      fired.insert(fired_key(match.ai, match.ri));
      add_trace(event, match.automation->id, match.rule->id, "condition",
                "true");
      bool rule_fault{};
      std::vector<AutomationEffect> candidate;
      for (const auto &action : match.rule->actions) {
        AutomationEffect effect;
        effect.kind = action.kind;
        effect.automation_id = match.automation->id;
        effect.rule_id = match.rule->id;
        effect.target = action.target;
        effect.name = action.name;
        effect.property = action.property;
        effect.timestamp = event.timestamp;
        effect.repeating = action.repeating;
        effect.priority = match.rule->priority;
        effect.automation_order = match.ai;
        effect.rule_order = match.ri;
        const bool needs_value = action_accepts_value(action.kind);
        if (needs_value) {
          auto value = eval(action.value, event, root_snapshot,
                            result.node_evaluations, error);
          if (!value) {
            rule_fault = true;
            break;
          }
          effect.value = std::move(*value);
        }
        if (action.secondary.kind != ExpressionKind::literal ||
            !std::holds_alternative<std::nullptr_t>(
                action.secondary.literal.storage)) {
          auto secondary = eval(action.secondary, event, root_snapshot,
                                result.node_evaluations, error);
          if (!secondary) {
            rule_fault = true;
            break;
          }
          effect.secondary = std::move(*secondary);
        }
        candidate.push_back(std::move(effect));
      }
      if (rule_fault) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      auto candidate_state = staged;
      auto candidate_winners = winners;
      std::vector<AutomationEvent> candidate_derived;
      for (auto &effect : candidate) {
        const auto key = effect_key(effect);
        const auto found = candidate_winners.find(key);
        if (found != candidate_winners.end() &&
            !effect_precedes(effect, found->second))
          continue;
        std::vector<AutomationEvent> derived;
        if (!apply_internal(effect, event.timestamp, candidate_state,
                            error, derived)) {
          rule_fault = true;
          break;
        }
        candidate_winners.insert_or_assign(key, effect);
        for (auto &next : derived) {
          next.origin_token = root.origin_token;
          candidate_derived.push_back(std::move(next));
        }
      }
      if (rule_fault) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      const auto derived_count = queue.size() - 1U;
      if (candidate_derived.size() >
          max_automation_derived_events - derived_count) {
        const AutomationError cap_fault{"$runtime",
                                        "root event exceeded 32 derived events"};
        add_trace(event, match.automation->id, match.rule->id, "error",
                  cap_fault.message);
        fault = cap_fault;
        return match.automation->id;
      }
      staged = std::move(candidate_state);
      winners = std::move(candidate_winners);
      for (auto &next : candidate_derived)
        queue.push_back(std::move(next));
    }
  }
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
  std::ranges::sort(result.effects, effect_precedes);
  AutomationRuntimeSnapshot committed_state = root_snapshot;
  for (const auto &effect : result.effects) {
    std::vector<AutomationEvent> ignored;
    AutomationError apply_error;
    if (!apply_internal(effect, effect.timestamp, committed_state,
                        apply_error, ignored)) {
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
std::vector<AutomationEvent>
AutomationRuntime::due_timer_events(TimePoint now) {
  std::vector<AutomationEvent> events;
  std::ranges::sort(impl_->state.timers, {}, &AutomationTimer::due);
  for (auto &timer : impl_->state.timers) {
    if (timer.due > now || events.size() >= max_automation_derived_events)
      continue;
    AutomationEvent event;
    event.kind = AutomationEventKind::timer_fired;
    event.timestamp = now;
    event.source_id = timer.id;
    event.fields.emplace(
        "lateBy",
        AutomationValue(DurationValue{std::max(
            Milliseconds::zero(),
            std::chrono::duration_cast<Milliseconds>(now - timer.due))}));
    events.push_back(std::move(event));
    if (timer.repeating)
      timer.due = now + timer.interval;
  }
  std::erase_if(impl_->state.timers, [&](const auto &timer) {
    return !timer.repeating && timer.due <= now;
  });
  std::erase_if(impl_->state.overlay_overrides, [&](const auto &item) {
    return item.expires_at && *item.expires_at <= now;
  });
  return events;
}
std::optional<TimePoint> AutomationRuntime::next_timer_due() const noexcept {
  std::optional<TimePoint> result;
  for (const auto &timer : impl_->state.timers)
    if (!result || timer.due < *result)
      result = timer.due;
  for (const auto &item : impl_->state.overlay_overrides)
    if (item.expires_at && (!result || *item.expires_at < *result))
      result = item.expires_at;
  return result;
}
void AutomationRuntime::reconfigure(
    AutomationProfile profile,
    const std::span<const CooldownGroup> cooldown_groups) {
  AutomationRuntimeSnapshot next;
  for (const auto &variable : profile.variables) {
    auto value = variable.default_value;
    const auto old = impl_->state.variables.find(variable.id);
    if (old != impl_->state.variables.end() &&
        value_type(old->second) == variable.type)
      value = old->second;
    next.variables[variable.id] = value;
    if (variable.persistent)
      next.persistent_values[variable.id] = value;
  }
  std::set<std::string, std::less<>> retained;
  for (const auto &automation : profile.automations) {
    const auto previous = std::ranges::find(
        impl_->profile.automations, automation.id, &AutomationDefinition::id);
    if (previous != impl_->profile.automations.end() && *previous == automation)
      retained.insert(automation.id);
  }
  for (const auto &timer : impl_->state.timers)
    if (retained.contains(timer.owner_automation))
      next.timers.push_back(timer);
  for (const auto &item : impl_->state.overlay_overrides)
    if (retained.contains(item.owner_automation))
      next.overlay_overrides.push_back(item);
  impl_->profile = std::move(profile);
  impl_->cooldown_groups.assign(cooldown_groups.begin(), cooldown_groups.end());
  impl_->state = std::move(next);
  // A profile switch invalidates the daemon's previous ready/value
  // observations: re-baseline the edge monitors with the new profile.
  impl_->edges = {};
  impl_->faulted.clear();
}
void AutomationRuntime::apply_persistent_values(
    const AutomationPersistentValues &values) {
  for (const auto &variable : impl_->profile.variables) {
    if (!variable.persistent)
      continue;
    const auto found = values.find(variable.id);
    if (found == values.end() || value_type(found->second) != variable.type ||
        !automation_value_valid(found->second))
      continue;
    impl_->state.variables[variable.id] = found->second;
    impl_->state.persistent_values[variable.id] = found->second;
  }
  // Deliberately does not clear persistent_dirty: values pushed in from a
  // sibling session are not a durable write of THIS runtime's snapshot, and
  // clearing here would drop this session's unsaved-edit marker (silent data
  // loss). Only mark_persistent_saved() after a durable write may clear it;
  // a stale true flag just re-pushes identical values on the next sync.
}
void AutomationRuntime::retry(std::string_view id) {
  impl_->faulted.erase(std::string(id));
}
void AutomationRuntime::clear_session_state() noexcept {
  impl_->state.timers.clear();
  impl_->state.overlay_overrides.clear();
  impl_->edges = {};
  for (const auto &variable : impl_->profile.variables) {
    auto value = variable.default_value;
    if (variable.persistent) {
      const auto found = impl_->state.persistent_values.find(variable.id);
      if (found != impl_->state.persistent_values.end())
        value = found->second;
    }
    impl_->state.variables[variable.id] = std::move(value);
  }
  impl_->faulted.clear();
}
bool AutomationRuntime::persistent_dirty() const noexcept {
  return impl_->persistent_dirty;
}
void AutomationRuntime::mark_persistent_saved() noexcept {
  impl_->persistent_dirty = false;
}
std::string AutomationRuntime::persistent_state_json() const {
  return serialize_automation_state_json(impl_->profile,
                                         impl_->state.persistent_values);
}

const std::vector<AutomationTraceEntry> &
AutomationRuntime::trace() const noexcept {
  return impl_->trace_entries;
}
void AutomationRuntime::set_trace_enabled(bool enabled) {
  impl_->trace_enabled = enabled;
  if (!enabled)
    impl_->trace_entries.clear();
}
void AutomationRuntime::consume_trace(const std::size_t count) {
  const auto consumed = std::min(count, impl_->trace_entries.size());
  impl_->trace_entries.erase(impl_->trace_entries.begin(),
                             impl_->trace_entries.begin() +
                                 static_cast<std::ptrdiff_t>(consumed));
}
void AutomationRuntime::clear_trace() { impl_->trace_entries.clear(); }
AutomationReplayResult replay_automation_events(
    const AutomationProfile &profile,
    const std::span<const AutomationEvent> events,
    const std::span<const AutomationReplayAssertion> assertions,
    const std::span<const CooldownGroup> cooldown_groups) {
  AutomationRuntime runtime(profile, {}, cooldown_groups);
  AutomationReplayResult result;
  for (std::size_t index{}; index < events.size(); ++index) {
    const auto evaluation = runtime.process(events[index]);
    if (!evaluation.committed)
      result.failures.push_back(
          "event " + std::to_string(index) + " failed: " +
          (evaluation.error.has_value() ? evaluation.error->message
                                        : "commit rejected"));
    ++result.processed_events;
  }
  result.final_state = runtime.snapshot();
  for (const auto &assertion : assertions) {
    const auto found = result.final_state.variables.find(assertion.variable_id);
    if (found == result.final_state.variables.end() ||
        found->second != assertion.expected)
      result.failures.push_back("variable assertion failed: " +
                                assertion.variable_id);
  }
  result.passed = result.failures.empty();
  return result;
}
namespace {

void append_trace_value(std::string &output, const AutomationValue &value) {
  output += "{\"type\":";
  json::append_quoted(
      output, detail::automation_value_type_name(value_type(value)));
  output += ",\"value\":";
  if (const auto *list = std::get_if<AutomationList>(&value.storage)) {
    output.push_back('[');
    for (std::size_t index{}; index < list->size(); ++index) {
      if (index != 0)
        output.push_back(',');
      append_trace_value(output, (*list)[index]);
    }
    output.push_back(']');
  } else if (const auto *map = std::get_if<AutomationMap>(&value.storage)) {
    output.push_back('{');
    std::size_t index{};
    for (const auto &[key, child] : *map) {
      if (index++ != 0)
        output.push_back(',');
      json::append_quoted(output, key);
      output.push_back(':');
      append_trace_value(output, child);
    }
    output.push_back('}');
  } else {
    output += automation_value_json(value);
  }
  output.push_back('}');
}

std::optional<AutomationValue> read_trace_value(const json::Value &entry,
                                                const std::string &path,
                                                const std::size_t depth,
                                                AutomationError &error) {
  if (depth > max_automation_ast_depth) {
    error = {path, "trace value nesting exceeds 16"};
    return std::nullopt;
  }
  if (!entry.is_object()) {
    error = {path, "trace value must be a typed object"};
    return std::nullopt;
  }
  const auto *type = entry.find("type");
  const auto *value = entry.find("value");
  if (type == nullptr || !type->is_string() || value == nullptr) {
    error = {path, "trace value requires type and value"};
    return std::nullopt;
  }
  const auto fail = [&](std::string message) -> std::optional<AutomationValue> {
    error = {path, std::move(message)};
    return std::nullopt;
  };
  const auto &name = type->as_string();
  if (name == "null")
    return value->is_null() ? std::optional(AutomationValue{})
                            : fail("expected null");
  if (name == "bool")
    return value->is_bool() ? std::optional(AutomationValue(value->as_bool()))
                            : fail("expected Boolean");
  if (name == "int" || name == "duration") {
    const auto parsed_integer = detail::read_exact_integer(*value);
    if (!parsed_integer.has_value())
      return fail("expected exact signed integer");
    const auto integer = *parsed_integer;
    if (name == "duration") {
      if (integer < 0)
        return fail("duration cannot be negative");
      return AutomationValue(DurationValue{Milliseconds(integer)});
    }
    return AutomationValue(integer);
  }
  if (name == "number" || name == "confidence") {
    if (!value->is_number() || !std::isfinite(value->as_number()))
      return fail("expected finite number");
    if (name == "confidence") {
      if (value->as_number() < 0.0 || value->as_number() > 1.0)
        return fail("confidence must be between 0 and 1");
      return AutomationValue(ConfidenceValue{value->as_number()});
    }
    return AutomationValue(value->as_number());
  }
  if (name == "string") {
    if (!value->is_string() ||
        value->as_string().size() > max_automation_string_bytes)
      return fail("expected bounded string");
    return AutomationValue(value->as_string());
  }
  if (name == "list") {
    if (!value->is_array() ||
        value->as_array().size() > max_automation_collection_entries)
      return fail("expected bounded list");
    AutomationList list;
    list.reserve(value->as_array().size());
    for (std::size_t index{}; index < value->as_array().size(); ++index) {
      auto child = read_trace_value(
          value->as_array()[index],
          path + ".value[" + std::to_string(index) + "]", depth + 1U, error);
      if (!child.has_value())
        return std::nullopt;
      list.push_back(std::move(*child));
    }
    return AutomationValue(std::move(list));
  }
  if (name == "map") {
    if (!value->is_object() ||
        value->as_object().size() > max_automation_collection_entries)
      return fail("expected bounded map");
    AutomationMap map;
    for (const auto &[key, child_value] : value->as_object()) {
      if (key.size() > max_automation_string_bytes)
        return fail("map key exceeds 512 UTF-8 bytes");
      auto child = read_trace_value(child_value, path + ".value." + key,
                                    depth + 1U, error);
      if (!child.has_value())
        return std::nullopt;
      map.emplace(key, std::move(*child));
    }
    return AutomationValue(std::move(map));
  }
  return fail("unsupported trace value type");
}

} // namespace

std::string
export_layerc_trace(const std::span<const AutomationTraceEntry> entries) {
  std::string output =
      "{\"schemaVersion\":1,\"kind\":\"layerctrace\",\"entries\":[";
  for (std::size_t index{}; index < entries.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const auto &entry = entries[index];
    output += "{\"sequence\":";
    detail::append_exact_unsigned(output, entry.sequence);
    output += ",\"timeMs\":";
    detail::append_exact_unsigned(
        output, static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, entry.timestamp.count())));
    output += ",\"automation\":";
    json::append_quoted(output, entry.automation_id);
    output += ",\"rule\":";
    json::append_quoted(output, entry.rule_id);
    output += ",\"phase\":";
    json::append_quoted(output, entry.phase);
    output += ",\"detail\":";
    json::append_quoted(output, entry.detail);
    if (entry.source_event.has_value()) {
      output += ",\"event\":";
      json::append_quoted(
      output, detail::automation_event_name(entry.source_event->kind));
      output += ",\"source\":";
      json::append_quoted(output, entry.source_event->source_id);
      output += ",\"origin\":";
      detail::append_exact_unsigned(output, entry.source_event->origin_token);
      output += ",\"fields\":{";
      std::size_t field_index{};
      for (const auto &[name, value] : entry.source_event->fields) {
        if (field_index++ != 0)
          output.push_back(',');
        json::append_quoted(output, name);
        output.push_back(':');
        append_trace_value(output, value);
      }
      output.push_back('}');
    }
    output.push_back('}');
  }
  output += "]}\n";
  return output;
}

std::size_t
live_trace_prefix_count(const LiveMessage &base_message,
                        const std::span<const AutomationTraceEntry> entries) {
  if (entries.empty())
    return 0U;
  auto message = base_message;
  std::size_t low = 1U;
  std::size_t high = entries.size();
  std::size_t selected{};
  while (low <= high) {
    const auto middle = low + (high - low) / 2U;
    message.text = export_layerc_trace(entries.first(middle));
    if (encode_live_message_json(message).size() <= max_live_message_bytes) {
      selected = middle;
      low = middle + 1U;
    } else {
      high = middle - 1U;
    }
  }
  return selected;
}

AutomationTraceParseResult parse_layerc_trace(const std::string_view source) {
  if (source.size() > max_automation_trace_bytes)
    return AutomationError{"$", "trace exceeds the 4 MiB limit"};
  const auto parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed))
    return AutomationError{"$@" + std::to_string(parse_error->offset),
                           parse_error->message};
  const auto &root = std::get<json::Value>(parsed);
  const auto *schema = root.is_object() ? root.find("schemaVersion") : nullptr;
  const auto *kind = root.is_object() ? root.find("kind") : nullptr;
  const auto *entries = root.is_object() ? root.find("entries") : nullptr;
  if (schema == nullptr || !schema->is_number() || schema->as_number() != 1.0 ||
      kind == nullptr || !kind->is_string() ||
      kind->as_string() != "layerctrace" || entries == nullptr ||
      !entries->is_array())
    return AutomationError{"$", "invalid LayerC trace document"};
  if (entries->as_array().size() > 2048U)
    return AutomationError{"$.entries", "trace exceeds 2048 entries"};

  std::vector<AutomationEvent> events;
  for (std::size_t index{}; index < entries->as_array().size(); ++index) {
    const auto &entry = entries->as_array()[index];
    if (!entry.is_object())
      return AutomationError{"$.entries[" + std::to_string(index) + "]",
                             "trace entry must be an object"};
    const auto *phase = entry.find("phase");
    if (phase == nullptr || !phase->is_string() ||
        phase->as_string() != "source")
      continue;
    const auto path = "$.entries[" + std::to_string(index) + "]";
    const auto *event_name_value = entry.find("event");
    const auto *time = entry.find("timeMs");
    const auto *source_id = entry.find("source");
    const auto *origin = entry.find("origin");
    const auto *fields = entry.find("fields");
    const auto time_value = time == nullptr
                                ? std::optional<std::uint64_t>{}
                                : detail::read_exact_unsigned(*time);
    const auto origin_value = origin == nullptr
                                  ? std::optional<std::uint64_t>{}
                                  : detail::read_exact_unsigned(*origin);
    if (event_name_value == nullptr || !event_name_value->is_string() ||
        !time_value.has_value() ||
        *time_value > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()) ||
        source_id == nullptr || !source_id->is_string() ||
        source_id->as_string().size() > max_automation_string_bytes ||
        !origin_value.has_value() || fields == nullptr ||
        !fields->is_object() ||
        fields->as_object().size() > max_automation_collection_entries)
      return AutomationError{path, "invalid source trace entry"};
    const auto event_kind =
        detail::parse_automation_event_name(event_name_value->as_string());
    if (!event_kind.has_value())
      return AutomationError{path + ".event", "unsupported event kind"};
    AutomationEvent event;
    event.kind = *event_kind;
    event.timestamp =
        TimePoint(Milliseconds(static_cast<std::int64_t>(*time_value)));
    event.source_id = source_id->as_string();
    event.origin_token = *origin_value;
    for (const auto &[name, value] : fields->as_object()) {
      if (name.size() > max_automation_string_bytes)
        return AutomationError{path + ".fields",
                               "field name exceeds 512 UTF-8 bytes"};
      AutomationError value_error;
      auto parsed_value =
          read_trace_value(value, path + ".fields." + name, 0, value_error);
      if (!parsed_value.has_value())
        return value_error;
      event.fields.emplace(name, std::move(*parsed_value));
    }
    events.push_back(std::move(event));
  }
  return events;
}
} // namespace layerc::core
