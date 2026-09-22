#include "automation_runtime_impl.hpp"

#include "automation_ops.hpp"

#include <algorithm>
#include <utility>

namespace layerc::core {
namespace {
using detail::truthy;

} // namespace

AutomationRuntime::Impl::Impl(AutomationProfile source,
                              AutomationPersistentValues persistent,
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

std::optional<AutomationValue> AutomationRuntime::Impl::eval(
    const AutomationExpression &expression, const AutomationEvent &event,
    const AutomationRuntimeSnapshot &snapshot, std::size_t &evaluations,
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

std::optional<std::string> AutomationRuntime::Impl::evaluate_event(
    const AutomationEvent &root, const AutomationEvent &event,
    EvaluationStage &stage) {
  add_source_trace(event);
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
    if (MonotonicClock::now() > stage.deadline) {
      const AutomationError timeout{"$runtime",
                                    "automation execution exceeded 2 ms"};
      add_trace(event, match.automation->id, match.rule->id, "error",
                timeout.message);
      stage.fault = timeout;
      return match.automation->id;
    }
    AutomationError error;
    auto condition = eval(match.rule->condition, event, stage.root_snapshot,
                          stage.result.node_evaluations, error);
    if (!condition) {
      add_trace(event, match.automation->id, match.rule->id, "error",
                error.message);
      stage.fault = error;
      return match.automation->id;
    }
    if (!truthy(*condition)) {
      add_trace(event, match.automation->id, match.rule->id, "condition",
                "false");
      continue;
    }
    stage.fired.insert(fired_key(match.ai, match.ri));
    add_trace(event, match.automation->id, match.rule->id, "condition", "true");
    bool rule_fault{};
    std::vector<AutomationEffect> candidate;
    for (const auto &action : match.rule->actions) {
      AutomationEffect effect;
      effect.kind = action.kind;
      effect.automation_id = match.automation->id;
      effect.target = action.target;
      effect.name = action.name;
      effect.property = action.property;
      effect.timestamp = event.timestamp;
      effect.repeating = action.repeating;
      effect.order = {match.rule->priority, match.ai, match.ri};
      if (action.value.has_value()) {
        auto value = eval(*action.value, event, stage.root_snapshot,
                          stage.result.node_evaluations, error);
        if (!value) {
          rule_fault = true;
          break;
        }
        effect.value = std::move(*value);
      }
      if (action.secondary.has_value()) {
        auto secondary = eval(*action.secondary, event, stage.root_snapshot,
                              stage.result.node_evaluations, error);
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
      stage.fault = error;
      return match.automation->id;
    }
    auto candidate_state = stage.staged;
    auto candidate_winners = stage.winners;
    std::vector<AutomationEvent> candidate_derived;
    for (auto &effect : candidate) {
      const auto key = runtime_effect_key(effect);
      const auto found = candidate_winners.find(key);
      if (found != candidate_winners.end() &&
          !runtime_effect_precedes(effect, found->second))
        continue;
      std::vector<AutomationEvent> derived;
      if (!apply_internal(effect, event.timestamp, candidate_state, error,
                          derived)) {
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
      stage.fault = error;
      return match.automation->id;
    }
    const auto derived_count = stage.queue.size() - 1U;
    if (candidate_derived.size() >
        max_automation_derived_events - derived_count) {
      const AutomationError cap_fault{"$runtime",
                                      "root event exceeded 32 derived events"};
      add_trace(event, match.automation->id, match.rule->id, "error",
                cap_fault.message);
      stage.fault = cap_fault;
      return match.automation->id;
    }
    stage.staged = std::move(candidate_state);
    stage.winners = std::move(candidate_winners);
    for (auto &next : candidate_derived)
      stage.queue.push_back(std::move(next));
  }

  return std::nullopt;
}

std::optional<std::string> AutomationRuntime::Impl::evaluate(
    const AutomationEvent &root, const EffectValidator &validator,
    const EffectCommitter &committer, const TimePoint deadline,
    AutomationEvaluationResult &result, AutomationError &fault) {
  result.effects.clear();
  result.derived_events.clear();
  result.committed = false;
  result.error.reset();
  // Every expression reads the root snapshot; only winners are committed.
  EvaluationStage stage{
      .root_snapshot = state,
      .staged = state,
      .queue = {root},
      .fired = {},
      .winners = {},
      .deadline = deadline,
      .result = result,
      .fault = fault,
  };
  std::size_t queue_index{};
  while (queue_index < stage.queue.size()) {
    if (queue_index >= max_automation_derived_events + 1U) {
      stage.result.error =
          AutomationError{"$runtime", "root event exceeded 32 derived events"};
      return std::nullopt;
    }
    const auto event = stage.queue[queue_index++];
    if (auto restart = evaluate_event(root, event, stage))
      return restart;
  }
  return commit_batch(stage, validator, committer);
}
} // namespace layerc::core
