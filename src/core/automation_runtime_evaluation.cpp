#include "automation_runtime_impl.hpp"

#include "automation_ops.hpp"

#include <algorithm>
#include <utility>

namespace layerc::core {
namespace {
using detail::truthy;

bool consume_timer_firing(AutomationRuntimeSnapshot &target,
                          const std::string_view id, const TimePoint now) {
  const auto timer = std::ranges::find(target.timers, id, &AutomationTimer::id);
  if (timer == target.timers.end() || timer->due > now)
    return true; // cancelled, replaced, or a firing that is not yet due
  if (timer->repeating) {
    const auto due = automation_time_after(now, timer->interval);
    if (!due)
      return false;
    timer->due = *due;
  } else
    target.timers.erase(timer);
  return true;
}

} // namespace

AutomationRuntime::Impl::Impl(AutomationProfile source,
                              AutomationPersistentValues persistent,
                              const std::span<const CooldownGroup> group_definitions,
                              ClockReader clock_reader)
    : profile(std::move(source)), groups(group_definitions),
      clock(std::move(clock_reader)) {
  for (const auto &variable : profile.variables)
    state.variables[variable.id] = variable.default_value;
  apply_persistent(persistent);
}

void AutomationRuntime::Impl::apply_persistent(
    const AutomationPersistentValues &values) {
  for (const auto &variable : profile.variables) {
    const auto found = values.find(variable.id);
    if (found != values.end() &&
        automation_persistent_value_accepted(variable, found->second))
      state.variables[variable.id] = found->second;
  }
}

ExpressionResult AutomationRuntime::Impl::eval(
    const AutomationExpression &expression, const AutomationEvent &event,
    const AutomationRuntimeSnapshot &snapshot, EvaluationBudget &budget) const {
  if (auto exhausted = budget.consume_node())
    return *exhausted;
  switch (expression.kind) {
  case ExpressionKind::literal:
    return expression.literal;
  case ExpressionKind::variable: {
    const auto found = snapshot.variables.find(expression.reference);
    if (found == snapshot.variables.end())
      return AutomationError{"$runtime",
                             "unknown variable " + expression.reference};
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
    auto target_result = eval(expression.arguments[0], event, snapshot, budget);
    auto *target = std::get_if<AutomationValue>(&target_result);
    if (!target)
      return target_result;
    auto key_result = eval(expression.arguments[1], event, snapshot, budget);
    const auto *key = std::get_if<AutomationValue>(&key_result);
    if (!key)
      return key_result;
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
    return AutomationError{"$runtime", "index target is not a list or map"};
  }
  default:
    break;
  }
  std::vector<AutomationValue> args;
  args.reserve(expression.arguments.size());
  for (const auto &argument : expression.arguments) {
    auto evaluated = eval(argument, event, snapshot, budget);
    auto *value = std::get_if<AutomationValue>(&evaluated);
    if (!value)
      return evaluated;
    args.push_back(std::move(*value));
  }
  if (auto exhausted = budget.check())
    return *exhausted;
  const auto *spec = detail::find_automation_op(expression.operation);
  if (spec == nullptr)
    return AutomationError{"$runtime", "unsupported expression operation"};
  AutomationValue evaluated;
  std::string message;
  if (!spec->evaluate(*spec, args, evaluated, message))
    return AutomationError{"$runtime", std::move(message)};
  return evaluated;
}

AttemptOutcome
AutomationRuntime::Impl::evaluate_event(const AutomationEvent &root,
                                        const AutomationEvent &event,
                                        EvaluationStage &stage) {
  struct Match {
    const AutomationDefinition *automation;
    const AutomationRule *rule;
    std::size_t ai;
    std::size_t ri;
  };
  const auto fired_key = [](const std::size_t ai, const std::size_t ri) {
    return (static_cast<std::uint64_t>(ai) << 32U) |
           static_cast<std::uint64_t>(ri);
  };
  std::vector<Match> matches;
  for (std::size_t ai{}; ai < profile.automations.size(); ++ai) {
    if (auto exhausted = stage.budget.check())
      return *exhausted;
    const auto &automation = profile.automations[ai];
    if (!automation.enabled || faulted.contains(automation.id))
      continue;
    for (std::size_t ri{}; ri < automation.rules.size(); ++ri) {
      const auto &rule = automation.rules[ri];
      if (rule.event == event.kind &&
          (rule.source_filter.empty() ||
           rule.source_filter == event.source_id) &&
          !stage.fired.contains(fired_key(ai, ri)))
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
    const auto fault_match = [&](AutomationError failure) -> AttemptOutcome {
      add_trace(event, match.automation->id, match.rule->id, "error",
                failure.message);
      return AutomationFault{match.automation->id, std::move(failure)};
    };
    const auto expression_failure =
        [&](const ExpressionResult &evaluated) -> AttemptOutcome {
      if (const auto *exhausted = std::get_if<BudgetExhausted>(&evaluated))
        return *exhausted;
      return fault_match(std::get<AutomationError>(evaluated));
    };
    if (auto exhausted = stage.budget.check())
      return *exhausted;
    auto evaluated =
        eval(match.rule->condition, event, stage.root_snapshot, stage.budget);
    const auto *condition = std::get_if<AutomationValue>(&evaluated);
    if (!condition)
      return expression_failure(evaluated);
    if (!truthy(*condition)) {
      add_trace(event, match.automation->id, match.rule->id, "condition",
                "false");
      continue;
    }
    stage.fired.insert(fired_key(match.ai, match.ri));
    add_trace(event, match.automation->id, match.rule->id, "condition", "true");
    std::vector<AutomationEffect> candidate;
    for (const auto &action : match.rule->actions) {
      if (auto exhausted = stage.budget.check())
        return *exhausted;
      AutomationEffect effect;
      effect.kind = action.kind;
      effect.automation_id = match.automation->id;
      effect.target = action.target;
      effect.name = action.name;
      effect.property = action.property;
      effect.timestamp = event.timestamp;
      effect.repeating = action.repeating;
      effect.order = {match.rule->priority, match.ai, match.ri};
      for (const auto &[expression, output] :
           {std::pair{&action.value, &effect.value},
            std::pair{&action.secondary, &effect.secondary}}) {
        if (!expression->has_value())
          continue;
        auto value =
            eval(**expression, event, stage.root_snapshot, stage.budget);
        auto *result = std::get_if<AutomationValue>(&value);
        if (!result)
          return expression_failure(value);
        *output = std::move(*result);
      }
      const auto *spec = automation_action_spec(action.kind);
      if (spec && (spec->value == ActionValuePolicy::match_variable ||
                   spec->value == ActionValuePolicy::match_numeric_variable ||
                   spec->value == ActionValuePolicy::overlay_property)) {
        const auto expected = automation_action_value_type(action, profile);
        if (expected && !normalize_automation_value(effect.value, *expected))
          return fault_match(
              {"$runtime", "action value does not match its declared type"});
      }
      candidate.push_back(std::move(effect));
    }
    auto candidate_state = stage.staged;
    auto candidate_winners = stage.winners;
    std::vector<AutomationEvent> candidate_derived;
    for (auto &effect : candidate) {
      if (auto exhausted = stage.budget.check())
        return *exhausted;
      const auto key = runtime_effect_key(effect);
      const auto found = candidate_winners.find(key);
      if (found != candidate_winners.end() &&
          !runtime_effect_precedes(effect, found->second))
        continue;
      std::vector<AutomationEvent> derived;
      AutomationError error;
      if (!apply_internal(effect, event.timestamp, candidate_state, error,
                          derived))
        return fault_match(std::move(error));
      candidate_winners.insert_or_assign(key, effect);
      for (auto &next : derived) {
        next.origin_token = root.origin_token;
        candidate_derived.push_back(std::move(next));
      }
    }
    const auto derived_count = stage.queue.size() - 1U;
    if (candidate_derived.size() >
        max_automation_derived_events - derived_count)
      return fault_match({"$runtime", "root event exceeded 32 derived events"});
    stage.staged = std::move(candidate_state);
    stage.winners = std::move(candidate_winners);
    for (auto &next : candidate_derived)
      stage.queue.push_back(std::move(next));
  }
  return AttemptComplete{};
}

AttemptOutcome AutomationRuntime::Impl::evaluate(
    const AutomationEvent &root, const EffectValidator &validator,
    const EffectCommitter &committer, EvaluationBudget &budget,
    AutomationEvaluationResult &result) {
  result.effects.clear();
  result.derived_events.clear();
  result.committed = false;
  result.error.reset();
  if (auto exhausted = budget.check())
    return *exhausted;
  // Consume the firing only in the transaction snapshot. Rejection leaves
  // the live timer due; accepted effects may replace or cancel it normally.
  auto root_snapshot = state;
  if (root.kind == AutomationEventKind::timer_fired &&
      !consume_timer_firing(root_snapshot, root.source_id, root.timestamp))
    return AttemptRejected{
        {"$runtime", "timer rearm is outside the clock range"}};
  // Every expression reads the root snapshot; only winners are committed.
  EvaluationStage stage{
      .root_snapshot = root_snapshot,
      .staged = std::move(root_snapshot),
      .queue = {root},
      .fired = {},
      .winners = {},
      .budget = budget,
      .result = result,
  };
  std::size_t queue_index{};
  while (queue_index < stage.queue.size()) {
    if (auto exhausted = budget.check())
      return *exhausted;
    if (queue_index >= max_automation_derived_events + 1U)
      return AttemptRejected{
          {"$runtime", "root event exceeded 32 derived events"}};
    const auto event = stage.queue[queue_index];
    if (queue_index++ != 0U)
      add_event_trace(event, "derived");
    auto outcome = evaluate_event(root, event, stage);
    if (!std::holds_alternative<AttemptComplete>(outcome))
      return outcome;
  }
  return commit_batch(stage, validator, committer);
}
} // namespace layerc::core
