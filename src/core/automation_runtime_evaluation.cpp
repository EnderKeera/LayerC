#include "automation_runtime_impl.hpp"

#include "automation_ops.hpp"

#include <algorithm>
#include <utility>

namespace layerc::core {
namespace {
using detail::truthy;

// Defense in depth for hand-assembled profiles: parsing validates these
// contracts, but the evaluator indexes argument vectors directly, so an
// expression with an unknown operator or out-of-range arity must never
// reach eval().
bool expression_within_contract(const AutomationExpression &expression,
                                const std::size_t depth) {
  if (depth > max_automation_ast_depth)
    return false;
  switch (expression.kind) {
  case ExpressionKind::literal:
  case ExpressionKind::variable:
  case ExpressionKind::event_field:
    return true;
  case ExpressionKind::index:
    if (expression.arguments.size() != 2U)
      return false;
    break;
  case ExpressionKind::unary:
  case ExpressionKind::binary:
  case ExpressionKind::call: {
    const auto *spec = detail::find_automation_op(expression.operation);
    if (spec == nullptr || spec->kind != expression.kind)
      return false;
    if (expression.arguments.size() < spec->min_args ||
        expression.arguments.size() > spec->max_args)
      return false;
    break;
  }
  }
  return std::ranges::all_of(expression.arguments, [&](const auto &child) {
    return expression_within_contract(child, depth + 1U);
  });
}
bool expressions_within_contract(const AutomationDefinition &automation) {
  return std::ranges::all_of(automation.rules, [](const auto &rule) {
    if (!expression_within_contract(rule.condition, 0))
      return false;
    return std::ranges::all_of(rule.actions, [](const auto &action) {
      return expression_within_contract(action.value, 0) &&
             expression_within_contract(action.secondary, 0);
    });
  });
}
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
  // Fail closed on out-of-contract expressions instead of evaluating them:
  // the faulted set keeps the session diagnosable and retry/reload clears
  // it once the profile is fixed.
  for (const auto &automation : profile.automations)
    if (!expressions_within_contract(automation))
      faulted.insert(automation.id);
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
    const AutomationRuntimeSnapshot &root_snapshot,
    AutomationRuntimeSnapshot &staged, std::vector<AutomationEvent> &queue,
    std::set<std::uint64_t> &fired,
    std::map<std::string, AutomationEffect, std::less<>> &winners,
    const TimePoint deadline, AutomationEvaluationResult &result,
    AutomationError &fault) {
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
  const auto root_snapshot = state;
  auto staged = root_snapshot;
  std::vector<AutomationEvent> queue{root};
  std::set<std::uint64_t> fired;
  std::map<std::string, AutomationEffect, std::less<>> winners;
  std::size_t queue_index{};
  while (queue_index < queue.size()) {
    if (queue_index >= max_automation_derived_events + 1U) {
      result.error =
          AutomationError{"$runtime", "root event exceeded 32 derived events"};
      return std::nullopt;
    }
    const auto event = queue[queue_index++];
    if (auto restart = evaluate_event(root, event, root_snapshot, staged, queue,
                                      fired, winners, deadline, result, fault))
      return restart;
  }
  return commit_batch(root_snapshot, queue, winners, validator, committer,
                      result, fault);
}
} // namespace layerc::core
