#include "layerc/core/automation.hpp"

#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "automation_profile_internal.hpp"
#include "utf8.hpp"

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

bool validate_expression_tree(
    const AutomationExpression &expression,
    const std::map<std::string, AutomationValueType, std::less<>> &variables,
    const std::string &path, std::size_t &nodes, const std::size_t depth,
    std::optional<AutomationValueType> &result, AutomationError &error) {
  if (++nodes > max_automation_nodes_per_rule) {
    error = {path, "rule exceeds 256 AST nodes"};
    return false;
  }
  if (depth > max_automation_ast_depth) {
    error = {path, "expression depth exceeds 16"};
    return false;
  }
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
        !detail::is_valid_utf8(expression.reference))
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
                                  nodes, depth + 1U, arguments[index], error))
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

using VariableTypes =
    std::map<std::string, AutomationValueType, std::less<>>;

bool is_null_expression(const AutomationExpression &expression) {
  return expression.kind == ExpressionKind::literal &&
         expression.arguments.empty() &&
         std::holds_alternative<std::nullptr_t>(expression.literal.storage);
}

bool is_compatible_type(const std::optional<AutomationValueType> actual,
                        const AutomationValueType expected) {
  return !actual.has_value() || *actual == expected ||
         (expected == AutomationValueType::number &&
          *actual == AutomationValueType::integer);
}

AutomationError validate_action(const AutomationAction &action,
                                const GameProfile &base,
                                const VariableTypes &variables,
                                const std::string &path, std::size_t &nodes) {
  const auto target_path = path + ".target";
  const bool requires_value = action_accepts_value(action.kind);
  const bool has_secondary = !is_null_expression(action.secondary);
  std::optional<AutomationValueType> value_type_result;
  std::optional<AutomationValueType> secondary_type;
  AutomationError expression_error;
  if (requires_value) {
    if (!validate_expression_tree(action.value, variables, path + ".value",
                                  nodes, 1, value_type_result,
                                  expression_error))
      return expression_error;
  } else if (!is_null_expression(action.value)) {
    return {path + ".value", "this action does not accept a value"};
  }
  if (has_secondary &&
      !validate_expression_tree(action.secondary, variables,
                                path + ".secondary", nodes, 1, secondary_type,
                                expression_error))
    return expression_error;
  if (action.repeating && action.kind != AutomationActionKind::create_timer)
    return {path + ".repeating", "only createTimer accepts repeating"};

  const auto variable = variables.find(action.target);
  switch (action.kind) {
  case AutomationActionKind::set_variable:
    if (variable == variables.end())
      return {target_path, "unknown variable ID"};
    if (!is_compatible_type(value_type_result, variable->second))
      return {path + ".value",
              "assignment type does not match the variable"};
    if (has_secondary)
      return {path + ".secondary", "setVariable does not accept secondary"};
    break;
  case AutomationActionKind::increment_variable:
    if (variable == variables.end())
      return {target_path, "unknown variable ID"};
    if (!numeric_type(variable->second) ||
        !is_compatible_type(value_type_result, variable->second))
      return {path + ".value",
              "incrementVariable must preserve the variable type"};
    if (has_secondary)
      return {path + ".secondary",
              "incrementVariable does not accept secondary"};
    break;
  case AutomationActionKind::append_list:
    if (variable == variables.end() ||
        variable->second != AutomationValueType::list)
      return {target_path, "appendList requires a list variable"};
    if (has_secondary)
      return {path + ".secondary", "appendList does not accept secondary"};
    break;
  case AutomationActionKind::set_map_entry:
    if (variable == variables.end() ||
        variable->second != AutomationValueType::map)
      return {target_path, "setMapEntry requires a map variable"};
    if (!has_secondary ||
        !is_compatible_type(secondary_type, AutomationValueType::string))
      return {path + ".secondary", "setMapEntry requires a string key"};
    break;
  case AutomationActionKind::erase_map_entry:
    if (variable == variables.end() ||
        variable->second != AutomationValueType::map)
      return {target_path, "eraseMapEntry requires a map variable"};
    if (!is_compatible_type(value_type_result, AutomationValueType::string))
      return {path + ".value", "eraseMapEntry requires a string key"};
    if (has_secondary)
      return {path + ".secondary",
              "eraseMapEntry does not accept secondary"};
    break;
  case AutomationActionKind::trigger_cooldown:
  case AutomationActionKind::reset_cooldown:
  case AutomationActionKind::refund_cooldown:
    if (!cooldown_exists(base, action.target))
      return {target_path, "unknown cooldown ID"};
    if (has_secondary)
      return {path + ".secondary",
              "cooldown action does not accept secondary"};
    break;
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
    if (!cooldown_exists(base, action.target))
      return {target_path, "unknown cooldown ID"};
    if (value_type_result.has_value() &&
        *value_type_result != AutomationValueType::duration &&
        *value_type_result != AutomationValueType::integer)
      return {path + ".value", "cooldown reduction requires a duration"};
    if (has_secondary)
      return {path + ".secondary",
              "cooldown reduction does not accept secondary"};
    break;
  case AutomationActionKind::trigger_group:
  case AutomationActionKind::reset_group:
    if (!group_exists(base, action.target))
      return {target_path, "unknown cooldown group ID"};
    if (has_secondary)
      return {path + ".secondary", "group action does not accept secondary"};
    break;
  case AutomationActionKind::create_timer:
    if (!detail::valid_automation_id(action.target))
      return {target_path, "invalid timer ID"};
    if (value_type_result.has_value() &&
        *value_type_result != AutomationValueType::duration &&
        *value_type_result != AutomationValueType::integer)
      return {path + ".value", "createTimer requires a duration"};
    if (has_secondary)
      return {path + ".secondary", "createTimer does not accept secondary"};
    break;
  case AutomationActionKind::cancel_timer:
    if (!detail::valid_automation_id(action.target))
      return {target_path, "invalid timer ID"};
    if (has_secondary)
      return {path + ".secondary", "cancelTimer does not accept secondary"};
    break;
  case AutomationActionKind::set_overlay_override: {
    if (!panel_exists(base, action.target))
      return {target_path, "unknown panel ID"};
    if (!detail::valid_automation_id(action.name))
      return {path + ".name", "overlay overrides require a valid name"};
    const auto property_type = overlay_property_type(action.property);
    if (!property_type.has_value())
      return {path + ".property", "unsupported overlay property"};
    if (!is_compatible_type(value_type_result, *property_type))
      return {path + ".value",
              "overlay value does not match the property type"};
    if (has_secondary && secondary_type.has_value() &&
        *secondary_type != AutomationValueType::duration &&
        *secondary_type != AutomationValueType::integer)
      return {path + ".secondary", "overlay expiry must be a duration"};
    break;
  }
  case AutomationActionKind::clear_overlay_override:
    if (!detail::valid_automation_id(action.target))
      return {target_path, "invalid overlay override name"};
    if (has_secondary)
      return {path + ".secondary",
              "clearOverlayOverride does not accept secondary"};
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
        !detail::is_valid_utf8(automation.name))
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
          !detail::is_valid_utf8(rule.source_filter))
        return {path + ".source",
                "source must be valid UTF-8 within 512 bytes"};
      if (rule.actions.empty() ||
          rule.actions.size() > max_automation_actions_per_rule)
        return {path + ".actions", "rules require 1 through 32 actions"};

      std::size_t nodes{};
      AutomationError expression_error;
      std::optional<AutomationValueType> condition_type;
      if (!validate_expression_tree(rule.condition, variables,
                                    path + ".condition", nodes, 1,
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
                                         variables, action_path, nodes);
            !error.message.empty())
          return error;
      }
    }
  }
  return {};
}
} // namespace layerc::core
