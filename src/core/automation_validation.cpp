#include "layerc/core/automation.hpp"

#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "automation_profile_internal.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <ranges>
#include <set>

namespace layerc::core {
namespace {
using detail::numeric_type;

bool panel_exists(const GameProfile &profile, const std::string_view id) {
  return std::ranges::any_of(
      profile.panels, [&](const Panel &panel) { return panel.id == id; });
}

std::optional<AutomationValueType>
overlay_property_type(const std::string_view property) {
  const auto *spec = find_overlay_property(property);
  return spec == nullptr ? std::nullopt : std::optional(spec->type);
}
bool cooldown_exists(const GameProfile &profile, std::string_view id) {
  return std::ranges::any_of(profile.cooldowns,
                             [&](const auto &item) { return item.id == id; });
}
bool group_exists(const GameProfile &profile, std::string_view id) {
  return std::ranges::any_of(profile.cooldown_groups,
                             [&](const auto &item) { return item.id == id; });
}

struct AstBudget {
  std::size_t nodes{};

  [[nodiscard]] bool consume(const std::string &path,
                             const std::size_t depth,
                             AutomationError &error) {
    if (++nodes > max_automation_nodes_per_rule) {
      error = {path, "rule exceeds 256 AST nodes"};
      return false;
    }
    if (depth > max_automation_ast_depth) {
      error = {path, "expression depth exceeds 16"};
      return false;
    }
    return true;
  }
};

using VariableTypes =
    std::map<std::string, AutomationValueType, std::less<>>;

bool validate_expression_tree(
    const AutomationExpression &expression, const VariableTypes &variables,
    const std::string &path, AstBudget &budget, const std::size_t depth,
    std::optional<AutomationValueType> &result, AutomationError &error) {
  if (!budget.consume(path, depth, error))
    return false;
  const auto fail = [&](std::string message) {
    error = {path, std::move(message)};
    return false;
  };

  switch (expression.kind) {
  case ExpressionKind::literal:
    if (!expression.arguments.empty() ||
        !automation_value_valid(expression.literal))
      return fail("invalid literal expression");
    result = value_type(expression.literal);
    return true;
  case ExpressionKind::variable: {
    if (!expression.arguments.empty() ||
        !detail::valid_automation_id(expression.reference))
      return fail("invalid variable reference");
    const auto variable = variables.find(expression.reference);
    if (variable == variables.end())
      return fail("unknown variable reference");
    result = variable->second;
    return true;
  }
  case ExpressionKind::event_field:
    if (!expression.arguments.empty() || expression.reference.empty() ||
        expression.reference.size() > max_automation_string_bytes ||
        !is_valid_utf8(expression.reference))
      return fail("invalid event field reference");
    if (expression.reference == "sourceId")
      result = AutomationValueType::string;
    else if (expression.reference == "ageMs" ||
             expression.reference == "lateBy")
      result = AutomationValueType::duration;
    else if (expression.reference == "available" ||
             expression.reference == "matched" ||
             expression.reference == "pressed")
      result = AutomationValueType::boolean;
    else if (expression.reference == "confidence")
      result = AutomationValueType::confidence;
    else
      result.reset();
    return true;
  default:
    break;
  }

  std::vector<std::optional<AutomationValueType>> arguments(
      expression.arguments.size());
  for (std::size_t index{}; index < expression.arguments.size(); ++index)
    if (!validate_expression_tree(expression.arguments[index], variables,
                                  path + ".args[" + std::to_string(index) + "]",
                                  budget, depth + 1U, arguments[index], error))
      return false;

  const auto known = [&](const std::size_t index,
                         const AutomationValueType expected) {
    return !arguments[index].has_value() || *arguments[index] == expected;
  };

  if (expression.kind == ExpressionKind::index) {
    if (!expression.operation.empty() || expression.arguments.size() != 2U)
      return fail("index expressions require target and key");
    if (arguments[0] == AutomationValueType::list &&
        !known(1, AutomationValueType::integer))
      return fail("list indices must be integers");
    if (arguments[0] == AutomationValueType::map &&
        !known(1, AutomationValueType::string))
      return fail("map indices must be strings");
    if (arguments[0].has_value() &&
        *arguments[0] != AutomationValueType::list &&
        *arguments[0] != AutomationValueType::map)
      return fail("index target must be a list or map");
    result.reset();
    return true;
  }

  const auto *spec = detail::find_automation_op(expression.operation);
  if (spec == nullptr || spec->kind != expression.kind)
    return fail("unsupported expression operator");
  if (expression.kind == ExpressionKind::unary &&
      expression.arguments.size() != 1U)
    return fail("unary operator requires one argument");
  if (expression.kind == ExpressionKind::binary &&
      expression.arguments.size() != 2U)
    return fail("binary operator requires two arguments");
  std::string message;
  if (!spec->infer(*spec, arguments, result, message))
    return fail(message);
  return true;
}

bool is_compatible_type(const std::optional<AutomationValueType> actual,
                        const AutomationValueType expected) {
  return !actual.has_value() || *actual == expected ||
         (expected == AutomationValueType::number &&
          *actual == AutomationValueType::integer);
}

bool duration_like(const std::optional<AutomationValueType> type) {
  return type.has_value() &&
         (*type == AutomationValueType::duration ||
          *type == AutomationValueType::integer);
}

AutomationError validate_action(const AutomationAction &action,
                                const GameProfile &base,
                                const VariableTypes &variables,
                                const std::string &path, AstBudget &budget) {
  const auto *spec = automation_action_spec(action.kind);
  if (spec == nullptr)
    return {path + ".type", "unsupported automation action"};
  const auto target_path = path + ".target";
  const bool has_value = action.value.has_value();
  const bool has_secondary = action.secondary.has_value();
  std::optional<AutomationValueType> primary_type;
  std::optional<AutomationValueType> secondary_type;
  AutomationError expression_error;
  if (spec->accepts_value()) {
    if (!has_value)
      return {path + ".value", "this action requires a value"};
    if (!validate_expression_tree(*action.value, variables, path + ".value",
                                  budget, 1, primary_type,
                                  expression_error))
      return expression_error;
  } else if (has_value) {
    return {path + ".value", "this action does not accept a value"};
  }
  switch (spec->secondary) {
  case ActionSecondaryPolicy::none:
    if (has_secondary)
      return {path + ".secondary", std::string(spec->wire_name) +
                                       " does not accept secondary"};
    break;
  case ActionSecondaryPolicy::required_string_key:
    if (!has_secondary)
      return {path + ".secondary", std::string(spec->wire_name) +
                                       " requires a string key"};
    if (!validate_expression_tree(*action.secondary, variables,
                                  path + ".secondary", budget, 1,
                                  secondary_type, expression_error))
      return expression_error;
    if (!is_compatible_type(secondary_type, AutomationValueType::string))
      return {path + ".secondary", std::string(spec->wire_name) +
                                       " requires a string key"};
    break;
  case ActionSecondaryPolicy::optional_duration:
    if (has_secondary) {
      if (!validate_expression_tree(*action.secondary, variables,
                                    path + ".secondary", budget, 1,
                                    secondary_type, expression_error))
        return expression_error;
      if (secondary_type.has_value() && !duration_like(secondary_type))
        return {path + ".secondary", "overlay expiry must be a duration"};
    }
    break;
  }
  if (action.repeating && !spec->allows_repeating)
    return {path + ".repeating", "only createTimer accepts repeating"};
  if (spec->requires_name && !detail::valid_automation_id(action.name))
    return {path + ".name", "overlay overrides require a valid name"};
  if (spec->requires_property) {
    const auto property_type = overlay_property_type(action.property);
    if (!property_type.has_value())
      return {path + ".property", "unsupported overlay property"};
    if (!is_compatible_type(primary_type, *property_type))
      return {path + ".value",
              "overlay value does not match the property type"};
  }

  const auto variable = variables.find(action.target);
  switch (spec->target) {
  case ActionTargetRole::variable_any:
    if (variable == variables.end())
      return {target_path, "unknown variable ID"};
    if (!is_compatible_type(primary_type, variable->second))
      return {path + ".value",
              "assignment type does not match the variable"};
    break;
  case ActionTargetRole::variable_numeric:
    if (variable == variables.end())
      return {target_path, "unknown variable ID"};
    if (!numeric_type(variable->second) ||
        !is_compatible_type(primary_type, variable->second))
      return {path + ".value",
              "incrementVariable must preserve the variable type"};
    break;
  case ActionTargetRole::variable_list:
    if (variable == variables.end() ||
        variable->second != AutomationValueType::list)
      return {target_path, "appendList requires a list variable"};
    break;
  case ActionTargetRole::variable_map:
    if (variable == variables.end() ||
        variable->second != AutomationValueType::map)
      return {target_path, std::string(spec->wire_name) +
                               " requires a map variable"};
    break;
  case ActionTargetRole::cooldown:
    if (!cooldown_exists(base, action.target))
      return {target_path, "unknown cooldown ID"};
    break;
  case ActionTargetRole::cooldown_group:
    if (!group_exists(base, action.target))
      return {target_path, "unknown cooldown group ID"};
    break;
  case ActionTargetRole::timer_id:
    if (!detail::valid_automation_id(action.target))
      return {target_path, "invalid timer ID"};
    break;
  case ActionTargetRole::panel:
    if (!panel_exists(base, action.target))
      return {target_path, "unknown panel ID"};
    break;
  case ActionTargetRole::override_name:
    if (!detail::valid_automation_id(action.target))
      return {target_path, "invalid overlay override name"};
    break;
  }

  switch (spec->value) {
  case ActionValuePolicy::none:
  case ActionValuePolicy::any:
  case ActionValuePolicy::match_variable:
  case ActionValuePolicy::match_numeric_variable:
  case ActionValuePolicy::overlay_property:
    break;
  case ActionValuePolicy::string_key:
    if (!is_compatible_type(primary_type, AutomationValueType::string))
      return {path + ".value", std::string(spec->wire_name) +
                                   " requires a string key"};
    break;
  case ActionValuePolicy::duration_or_integer:
    if (primary_type.has_value() && !duration_like(primary_type))
      return {path + ".value", std::string(spec->wire_name) +
                                   " requires a duration"};
    break;
  }
  return {};
}
} // namespace

AutomationError validate_automation_profile(const AutomationProfile &profile,
                                            const GameProfile &base) {
  if (profile.schema_version != 1)
    return {"$.schemaVersion", "unsupported automation schema version"};
  if (profile.profile_id != base.id)
    return {"$.profileId", "automation profile ID does not match base profile"};
  if (!std::isfinite(profile.continuous_input.deadzone) ||
      profile.continuous_input.deadzone < 0.0 ||
      profile.continuous_input.deadzone > 0.95)
    return {"$.continuousInput.deadzone", "expected 0 through 0.95"};
  if (!std::isfinite(profile.continuous_input.change_threshold) ||
      profile.continuous_input.change_threshold <= 0.0 ||
      profile.continuous_input.change_threshold > 1.0)
    return {"$.continuousInput.changeThreshold", "expected above 0 through 1"};
  if (profile.continuous_input.max_events_per_second == 0 ||
      profile.continuous_input.max_events_per_second > 240)
    return {"$.continuousInput.maxEventsPerSecond",
            "expected integer 1 through 240"};
  if (profile.variables.size() > max_automation_variables)
    return {"$.variables", "at most 64 variables are allowed"};

  VariableTypes variables;
  for (std::size_t index{}; index < profile.variables.size(); ++index) {
    const auto &variable = profile.variables[index];
    const auto path = "$.variables[" + std::to_string(index) + "]";
    if (!detail::valid_automation_id(variable.id) ||
        !variables.emplace(variable.id, variable.type).second)
      return {path + ".id", "duplicate or invalid variable ID"};
    if (variable.type == AutomationValueType::null_value ||
        value_type(variable.default_value) != variable.type ||
        !automation_value_valid(variable.default_value))
      return {path + ".default", "invalid variable default"};
  }

  std::set<std::string, std::less<>> automation_ids;
  std::size_t total_rules{};
  for (std::size_t automation_index{};
       automation_index < profile.automations.size(); ++automation_index) {
    const auto &automation = profile.automations[automation_index];
    const auto automation_path =
        "$.automations[" + std::to_string(automation_index) + "]";
    if (!detail::valid_automation_id(automation.id) ||
        !automation_ids.insert(automation.id).second)
      return {automation_path + ".id", "duplicate or invalid automation ID"};
    if (automation.name.size() > max_automation_string_bytes ||
        !is_valid_utf8(automation.name))
      return {automation_path + ".name",
              "name must be valid UTF-8 within 512 bytes"};
    total_rules += automation.rules.size();
    if (total_rules > max_automation_rules)
      return {"$.automations", "at most 64 rules are allowed"};

    std::set<std::string, std::less<>> rule_ids;
    for (std::size_t rule_index{}; rule_index < automation.rules.size();
         ++rule_index) {
      const auto &rule = automation.rules[rule_index];
      const auto path =
          automation_path + ".rules[" + std::to_string(rule_index) + "]";
      if (!detail::valid_automation_id(rule.id) ||
          !rule_ids.insert(rule.id).second)
        return {path + ".id", "duplicate or invalid rule ID"};
      if (rule.priority < -1'000'000 || rule.priority > 1'000'000)
        return {path + ".priority", "priority must be a bounded integer"};
      if (rule.source_filter.size() > max_automation_string_bytes ||
          !is_valid_utf8(rule.source_filter))
        return {path + ".source",
                "source must be valid UTF-8 within 512 bytes"};
      if (rule.actions.empty() ||
          rule.actions.size() > max_automation_actions_per_rule)
        return {path + ".actions", "rules require 1 through 32 actions"};

      AstBudget budget;
      AutomationError expression_error;
      std::optional<AutomationValueType> condition_type;
      if (!validate_expression_tree(rule.condition, variables,
                                    path + ".condition", budget, 1,
                                    condition_type, expression_error))
        return expression_error;
      if (condition_type.has_value() &&
          *condition_type != AutomationValueType::boolean)
        return {path + ".condition",
                "rule conditions must evaluate to Boolean"};

      for (std::size_t action_index{}; action_index < rule.actions.size();
           ++action_index) {
        const auto action_path =
            path + ".actions[" + std::to_string(action_index) + "]";
        if (auto error = validate_action(rule.actions[action_index], base,
                                         variables, action_path, budget);
            !error.message.empty())
          return error;
      }
    }
  }
  return {};
}
} // namespace layerc::core
