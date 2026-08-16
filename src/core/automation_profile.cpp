#include "layerc/core/automation.hpp"

#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "layerc/core/json.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <type_traits>
#include <utility>

namespace layerc::core {
namespace {
using detail::numeric_type;
using json::Value;
using detail::automation_event_name;
using detail::automation_value_type_name;
using detail::parse_automation_event_name;

std::optional<AutomationValueType> parse_type(std::string_view text) {
  if (text == "null")
    return AutomationValueType::null_value;
  if (text == "bool")
    return AutomationValueType::boolean;
  if (text == "int")
    return AutomationValueType::integer;
  if (text == "number")
    return AutomationValueType::number;
  if (text == "duration")
    return AutomationValueType::duration;
  if (text == "confidence")
    return AutomationValueType::confidence;
  if (text == "string")
    return AutomationValueType::string;
  if (text == "list")
    return AutomationValueType::list;
  if (text == "map")
    return AutomationValueType::map;
  return std::nullopt;
}
std::string_view action_name(AutomationActionKind kind) {
  switch (kind) {
  case AutomationActionKind::set_variable:
    return "setVariable";
  case AutomationActionKind::increment_variable:
    return "incrementVariable";
  case AutomationActionKind::append_list:
    return "appendList";
  case AutomationActionKind::set_map_entry:
    return "setMapEntry";
  case AutomationActionKind::erase_map_entry:
    return "eraseMapEntry";
  case AutomationActionKind::trigger_cooldown:
    return "triggerCooldown";
  case AutomationActionKind::reset_cooldown:
    return "resetCooldown";
  case AutomationActionKind::reduce_cooldown_next:
    return "reduceCooldownNext";
  case AutomationActionKind::reduce_cooldown_all:
    return "reduceCooldownAll";
  case AutomationActionKind::refund_cooldown:
    return "refundCooldown";
  case AutomationActionKind::trigger_group:
    return "triggerGroup";
  case AutomationActionKind::reset_group:
    return "resetGroup";
  case AutomationActionKind::create_timer:
    return "createTimer";
  case AutomationActionKind::cancel_timer:
    return "cancelTimer";
  case AutomationActionKind::set_overlay_override:
    return "setOverlayOverride";
  case AutomationActionKind::clear_overlay_override:
    return "clearOverlayOverride";
  }
  return "setVariable";
}
std::optional<AutomationActionKind> parse_action(std::string_view text) {
  for (auto kind :
       {AutomationActionKind::set_variable,
        AutomationActionKind::increment_variable,
        AutomationActionKind::append_list, AutomationActionKind::set_map_entry,
        AutomationActionKind::erase_map_entry,
        AutomationActionKind::trigger_cooldown,
        AutomationActionKind::reset_cooldown,
        AutomationActionKind::reduce_cooldown_next,
        AutomationActionKind::reduce_cooldown_all,
        AutomationActionKind::refund_cooldown,
        AutomationActionKind::trigger_group, AutomationActionKind::reset_group,
        AutomationActionKind::create_timer, AutomationActionKind::cancel_timer,
        AutomationActionKind::set_overlay_override,
        AutomationActionKind::clear_overlay_override})
    if (action_name(kind) == text)
      return kind;
  return std::nullopt;
}
bool valid_id(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.';
         });
}

bool panel_exists(const GameProfile &profile, const std::string_view id) {
  return std::ranges::any_of(
      profile.panels, [&](const Panel &panel) { return panel.id == id; });
}

std::optional<AutomationValueType>
overlay_property_type(const std::string_view property) {
  const auto *spec = find_overlay_property(property);
  return spec == nullptr ? std::nullopt : std::optional(spec->type);
}
void append_tagged_value(std::string &out, const AutomationValue &value);

void append_value(std::string &out, const AutomationValue &value) {
  std::visit(
      [&](const auto &entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>)
          out += "null";
        else if constexpr (std::is_same_v<T, bool>)
          out += entry ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::int64_t>) {
          detail::append_exact_integer(out, entry);
        } else if constexpr (std::is_same_v<T, double>)
          json::append_number(out, entry);
        else if constexpr (std::is_same_v<T, DurationValue>)
          json::append_number(out, static_cast<double>(entry.value.count()));
        else if constexpr (std::is_same_v<T, ConfidenceValue>)
          json::append_number(out, entry.value);
        else if constexpr (std::is_same_v<T, std::string>)
          json::append_quoted(out, entry);
        else if constexpr (std::is_same_v<T, AutomationList>) {
          out.push_back('[');
          for (std::size_t i{}; i < entry.size(); ++i) {
            if (i)
              out.push_back(',');
            append_tagged_value(out, entry[i]);
          }
          out.push_back(']');
        } else if constexpr (std::is_same_v<T, AutomationMap>) {
          out.push_back('{');
          std::size_t i{};
          for (const auto &[key, child] : entry) {
            if (i++)
              out.push_back(',');
            json::append_quoted(out, key);
            out.push_back(':');
            append_tagged_value(out, child);
          }
          out.push_back('}');
        }
      },
      value.storage);
}

void append_tagged_value(std::string &out, const AutomationValue &value) {
  out += "{\"$type\":";
  json::append_quoted(out, automation_value_type_name(value_type(value)));
  out += ",\"$value\":";
  append_value(out, value);
  out.push_back('}');
}

AutomationValueType infer_type(const Value &value) {
  if (value.is_null())
    return AutomationValueType::null_value;
  if (value.is_bool())
    return AutomationValueType::boolean;
  if (value.is_number())
    return AutomationValueType::number;
  if (value.is_string())
    return AutomationValueType::string;
  if (value.is_array())
    return AutomationValueType::list;
  return AutomationValueType::map;
}
std::optional<AutomationValue>
read_value(const Value &value, AutomationValueType type, AutomationError &error,
           const std::string &path, std::size_t depth = 0);

std::optional<AutomationValue> read_nested_value(const Value &value,
                                                 AutomationError &error,
                                                 const std::string &path,
                                                 const std::size_t depth) {
  if (value.is_object() && value.as_object().size() == 2U) {
    const auto *tag = value.find("$type");
    const auto *payload = value.find("$value");
    if (tag != nullptr && tag->is_string() && payload != nullptr) {
      const auto type = parse_type(tag->as_string());
      if (!type.has_value()) {
        error = {path + ".$type", "unsupported nested value type"};
        return std::nullopt;
      }
      return read_value(*payload, *type, error, path + ".$value", depth);
    }
  }
  return read_value(value, infer_type(value), error, path, depth);
}

std::optional<AutomationValue> read_value(const Value &value,
                                          const AutomationValueType type,
                                          AutomationError &error,
                                          const std::string &path,
                                          const std::size_t depth) {
  if (depth > max_automation_ast_depth) {
    error = {path, "value nesting exceeds 16"};
    return std::nullopt;
  }
  switch (type) {
  case AutomationValueType::null_value:
    if (value.is_null())
      return AutomationValue{};
    break;
  case AutomationValueType::boolean:
    if (value.is_bool())
      return AutomationValue(value.as_bool());
    break;
  case AutomationValueType::integer:
    if (const auto integer = detail::read_exact_integer(value))
      return AutomationValue(*integer);
    break;
  case AutomationValueType::number:
    if (value.is_number() && std::isfinite(value.as_number()))
      return AutomationValue(value.as_number());
    break;
  case AutomationValueType::duration:
    if (value.is_number() && std::isfinite(value.as_number()) &&
        std::floor(value.as_number()) == value.as_number() &&
        value.as_number() >= 0 && value.as_number() <= 86'400'000)
      return AutomationValue(DurationValue{
          Milliseconds(static_cast<std::int64_t>(value.as_number()))});
    break;
  case AutomationValueType::confidence:
    if (value.is_number() && std::isfinite(value.as_number()) &&
        value.as_number() >= 0 && value.as_number() <= 1)
      return AutomationValue(ConfidenceValue{value.as_number()});
    break;
  case AutomationValueType::string:
    if (value.is_string() &&
        value.as_string().size() <= max_automation_string_bytes)
      return AutomationValue(value.as_string());
    break;
  case AutomationValueType::list:
    if (value.is_array() &&
        value.as_array().size() <= max_automation_collection_entries) {
      AutomationList result;
      result.reserve(value.as_array().size());
      for (std::size_t i{}; i < value.as_array().size(); ++i) {
        auto child =
            read_nested_value(value.as_array()[i], error,
                              path + "[" + std::to_string(i) + "]", depth + 1);
        if (!child)
          return std::nullopt;
        result.push_back(std::move(*child));
      }
      return AutomationValue(std::move(result));
    }
    break;
  case AutomationValueType::map:
    if (value.is_object() &&
        value.as_object().size() <= max_automation_collection_entries) {
      AutomationMap result;
      for (const auto &[key, raw] : value.as_object()) {
        if (key.size() > max_automation_string_bytes) {
          error = {path, "map key exceeds 512 UTF-8 bytes"};
          return std::nullopt;
        }
        auto child = read_nested_value(raw, error, path + "." + key, depth + 1);
        if (!child)
          return std::nullopt;
        result.emplace(key, std::move(*child));
      }
      return AutomationValue(std::move(result));
    }
    break;
  }
  error = {path, "value does not match declared type " +
                     std::string(automation_value_type_name(type))};
  return std::nullopt;
}
struct Parser {
  const Value &root;
  const GameProfile &base;
  AutomationError error;
  bool fail(std::string path, std::string message) {
    if (error.message.empty())
      error = {std::move(path), std::move(message)};
    return false;
  }
  const Value *required(const Value &parent, std::string_view key,
                        const std::string &path) {
    const auto *value = parent.find(key);
    if (!value)
      fail(path + "." + std::string(key), "required value is missing");
    return value;
  }
  bool string(const Value &parent, std::string_view key,
              const std::string &path, std::string &out,
              bool optional = false) {
    const auto *value = parent.find(key);
    if (!value && optional)
      return true;
    if (!value || !value->is_string())
      return fail(path + "." + std::string(key), "expected string");
    if (value->as_string().size() > max_automation_string_bytes)
      return fail(path + "." + std::string(key),
                  "string exceeds 512 UTF-8 bytes");
    out = value->as_string();
    return true;
  }
  bool boolean(const Value &parent, std::string_view key,
               const std::string &path, bool &out, bool optional = false) {
    const auto *value = parent.find(key);
    if (!value && optional)
      return true;
    if (!value || !value->is_bool())
      return fail(path + "." + std::string(key), "expected Boolean");
    out = value->as_bool();
    return true;
  }
  bool expression(const Value &value, AutomationExpression &out,
                  const std::string &path, std::size_t &nodes,
                  std::size_t depth) {
    out = AutomationExpression{};

    if (++nodes > max_automation_nodes_per_rule)
      return fail(path, "rule exceeds 256 AST nodes");
    if (depth > max_automation_ast_depth)
      return fail(path, "expression depth exceeds 16");
    if (!value.is_object())
      return fail(path, "expression must be an object");
    std::string op;
    if (!string(value, "op", path, op))
      return false;
    if (op == "literal") {
      out.kind = ExpressionKind::literal;
      std::string declared;
      if (!string(value, "valueType", path, declared))
        return false;
      const auto type = parse_type(declared);
      const auto *literal = required(value, "value", path);
      if (!type || !literal)
        return fail(path, "literal requires a supported valueType and value");
      auto parsed = read_value(*literal, *type, error, path + ".value");
      if (!parsed)
        return false;
      out.literal = std::move(*parsed);
      return true;
    }
    if (op == "variable" || op == "event") {
      out.kind = op == "variable" ? ExpressionKind::variable
                                  : ExpressionKind::event_field;
      return string(value, op == "variable" ? "id" : "field", path,
                    out.reference);
    }
    if (op == "index") {
      out.kind = ExpressionKind::index;
      const auto *target = required(value, "target", path);
      const auto *key = required(value, "key", path);
      if (!target || !key)
        return false;
      out.arguments.resize(2);
      return expression(*target, out.arguments[0], path + ".target", nodes,
                        depth + 1) &&
             expression(*key, out.arguments[1], path + ".key", nodes,
                        depth + 1);
    }
    const auto *spec = detail::find_automation_op(op);
    if (spec == nullptr)
      return fail(path + ".op", "unsupported expression operator");
    out.kind = spec->kind;
    out.operation = op;
    const auto *args = required(value, "args", path);
    if (!args || !args->is_array())
      return fail(path + ".args", "expected array");
    const auto count = args->as_array().size();
    if (count < spec->min_args || count > spec->max_args)
      return fail(path + ".args", "operator has an invalid argument count");
    out.arguments.resize(args->as_array().size());
    for (std::size_t i{}; i < out.arguments.size(); ++i)
      if (!expression(args->as_array()[i], out.arguments[i],
                      path + ".args[" + std::to_string(i) + "]", nodes,
                      depth + 1))
        return false;
    return true;
  }
  bool action(const Value &value, AutomationAction &out,
              const std::string &path, std::size_t &nodes) {
    if (!value.is_object())
      return fail(path, "action must be an object");
    std::string kind;
    if (!string(value, "type", path, kind))
      return false;
    const auto parsed = parse_action(kind);
    if (!parsed)
      return fail(path + ".type", "unsupported automation action");
    out.kind = *parsed;
    if (!string(value, "target", path, out.target) ||
        !string(value, "name", path, out.name, true) ||
        !string(value, "property", path, out.property, true) ||
        !boolean(value, "repeating", path, out.repeating, true))
      return false;
    if (const auto *raw = value.find("value"))
      if (!expression(*raw, out.value, path + ".value", nodes, 1))
        return false;
    if (const auto *raw = value.find("secondary"))
      if (!expression(*raw, out.secondary, path + ".secondary", nodes, 1))
        return false;
    return true;
  }
  AutomationProfileResult parse() {
    if (!root.is_object())
      return AutomationError{"$", "automation root must be an object"};
    const auto *schema = required(root, "schemaVersion", "$");
    if (!schema || !schema->is_number() || schema->as_number() != 1)
      return AutomationError{"$.schemaVersion",
                             "unsupported automation schema version"};
    AutomationProfile profile;
    if (!string(root, "profileId", "$", profile.profile_id))
      return error;
    if (profile.profile_id != base.id)
      return AutomationError{
          "$.profileId", "automation profile ID does not match base profile"};
    if (const auto *input = root.find("continuousInput")) {
      if (!input->is_object())
        return AutomationError{"$.continuousInput", "expected object"};
      if (const auto *v = input->find("deadzone")) {
        if (!v->is_number() || v->as_number() < 0 || v->as_number() > .95)
          return AutomationError{"$.continuousInput.deadzone",
                                 "expected 0 through 0.95"};
        profile.continuous_input.deadzone = v->as_number();
      }
      if (const auto *v = input->find("changeThreshold")) {
        if (!v->is_number() || v->as_number() <= 0 || v->as_number() > 1)
          return AutomationError{"$.continuousInput.changeThreshold",
                                 "expected above 0 through 1"};
        profile.continuous_input.change_threshold = v->as_number();
      }
      if (const auto *v = input->find("maxEventsPerSecond")) {
        if (!v->is_number() || std::floor(v->as_number()) != v->as_number() ||
            v->as_number() < 1 || v->as_number() > 240)
          return AutomationError{"$.continuousInput.maxEventsPerSecond",
                                 "expected integer 1 through 240"};
        profile.continuous_input.max_events_per_second =
            static_cast<std::uint32_t>(v->as_number());
      }
    }
    const auto *variables = required(root, "variables", "$"),
               *automations = required(root, "automations", "$");
    if (!variables || !variables->is_array())
      return AutomationError{"$.variables", "expected array"};
    if (!automations || !automations->is_array())
      return AutomationError{"$.automations", "expected array"};
    if (variables->as_array().size() > max_automation_variables)
      return AutomationError{"$.variables", "at most 64 variables are allowed"};
    std::set<std::string, std::less<>> variable_ids;
    for (std::size_t i{}; i < variables->as_array().size(); ++i) {
      const auto path = "$.variables[" + std::to_string(i) + "]";
      const auto &entry = variables->as_array()[i];
      if (!entry.is_object())
        return AutomationError{path, "variable must be an object"};
      AutomationVariable variable;
      std::string declared;
      if (!string(entry, "id", path, variable.id) || !valid_id(variable.id))
        return error.message.empty()
                   ? AutomationError{path + ".id", "invalid variable ID"}
                   : error;
      if (!variable_ids.insert(variable.id).second)
        return AutomationError{path + ".id", "duplicate variable ID"};
      if (!string(entry, "type", path, declared) ||
          !boolean(entry, "persistent", path, variable.persistent, true))
        return error;
      const auto type = parse_type(declared);
      if (!type || *type == AutomationValueType::null_value)
        return AutomationError{path + ".type",
                               "variables require a non-null supported type"};
      variable.type = *type;
      const auto *raw = required(entry, "default", path);
      if (!raw)
        return error;
      auto parsed = read_value(*raw, variable.type, error, path + ".default");
      if (!parsed)
        return error;
      variable.default_value = std::move(*parsed);
      profile.variables.push_back(std::move(variable));
    }
    std::set<std::string, std::less<>> automation_ids;
    std::size_t total_rules{};
    for (std::size_t ai{}; ai < automations->as_array().size(); ++ai) {
      const auto path = "$.automations[" + std::to_string(ai) + "]";
      const auto &entry = automations->as_array()[ai];
      if (!entry.is_object())
        return AutomationError{path, "automation must be an object"};
      AutomationDefinition automation;
      if (!string(entry, "id", path, automation.id) || !valid_id(automation.id))
        return error.message.empty()
                   ? AutomationError{path + ".id", "invalid automation ID"}
                   : error;
      if (!automation_ids.insert(automation.id).second)
        return AutomationError{path + ".id", "duplicate automation ID"};
      if (!string(entry, "name", path, automation.name) ||
          !boolean(entry, "enabled", path, automation.enabled, true))
        return error;
      const auto *rules = required(entry, "rules", path);
      if (!rules || !rules->is_array())
        return AutomationError{path + ".rules", "expected array"};
      total_rules += rules->as_array().size();
      if (total_rules > max_automation_rules)
        return AutomationError{"$.automations", "at most 64 rules are allowed"};
      std::set<std::string, std::less<>> rule_ids;
      for (std::size_t ri{}; ri < rules->as_array().size(); ++ri) {
        const auto rule_path = path + ".rules[" + std::to_string(ri) + "]";
        const auto &entry_rule = rules->as_array()[ri];
        if (!entry_rule.is_object())
          return AutomationError{rule_path, "rule must be an object"};
        AutomationRule rule;
        std::string event;
        if (!string(entry_rule, "id", rule_path, rule.id) || !valid_id(rule.id))
          return error.message.empty()
                     ? AutomationError{rule_path + ".id", "invalid rule ID"}
                     : error;
        if (!rule_ids.insert(rule.id).second)
          return AutomationError{rule_path + ".id", "duplicate rule ID"};
        if (!string(entry_rule, "event", rule_path, event) ||
            !string(entry_rule, "source", rule_path, rule.source_filter, true))
          return error;
        const auto parsed_event = parse_automation_event_name(event);
        if (!parsed_event)
          return AutomationError{rule_path + ".event",
                                 "unsupported event source"};
        rule.event = *parsed_event;
        if (const auto *priority = entry_rule.find("priority")) {
          if (!priority->is_number() ||
              std::floor(priority->as_number()) != priority->as_number() ||
              priority->as_number() < -1'000'000 ||
              priority->as_number() > 1'000'000)
            return AutomationError{rule_path + ".priority",
                                   "priority must be a bounded integer"};
          rule.priority = static_cast<std::int32_t>(priority->as_number());
        }
        std::size_t nodes{};
        if (const auto *condition = entry_rule.find("condition"))
          if (!expression(*condition, rule.condition, rule_path + ".condition",
                          nodes, 1))
            return error;
        const auto *actions = required(entry_rule, "actions", rule_path);
        if (!actions || !actions->is_array())
          return AutomationError{rule_path + ".actions", "expected array"};
        if (actions->as_array().empty() ||
            actions->as_array().size() > max_automation_actions_per_rule)
          return AutomationError{rule_path + ".actions",
                                 "rules require 1 through 32 actions"};
        rule.actions.resize(actions->as_array().size());
        for (std::size_t i{}; i < rule.actions.size(); ++i)
          if (!action(actions->as_array()[i], rule.actions[i],
                      rule_path + ".actions[" + std::to_string(i) + "]", nodes))
            return error;
        automation.rules.push_back(std::move(rule));
      }
      profile.automations.push_back(std::move(automation));
    }
    const auto validation = validate_automation_profile(profile, base);
    if (!validation.message.empty())
      return validation;
    return profile;
  }
};

void append_expression(std::string &out,
                       const AutomationExpression &expression) {
  out += "{\"op\":";
  if (expression.kind == ExpressionKind::literal) {
    json::append_quoted(out, "literal");
    out += ",\"valueType\":";
    json::append_quoted(out, automation_value_type_name(value_type(expression.literal)));
    out += ",\"value\":";
    append_value(out, expression.literal);
  } else if (expression.kind == ExpressionKind::variable ||
             expression.kind == ExpressionKind::event_field) {
    json::append_quoted(out, expression.kind == ExpressionKind::variable
                                 ? "variable"
                                 : "event");
    out += expression.kind == ExpressionKind::variable ? ",\"id\":"
                                                       : ",\"field\":";
    json::append_quoted(out, expression.reference);
  } else if (expression.kind == ExpressionKind::index) {
    json::append_quoted(out, "index");
    out += ",\"target\":";
    append_expression(out, expression.arguments[0]);
    out += ",\"key\":";
    append_expression(out, expression.arguments[1]);
  } else {
    json::append_quoted(out, expression.operation);
    out += ",\"args\":[";
    for (std::size_t i{}; i < expression.arguments.size(); ++i) {
      if (i)
        out.push_back(',');
      append_expression(out, expression.arguments[i]);
    }
    out.push_back(']');
  }
  out.push_back('}');
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
  const auto require_count = [&](const std::size_t count) {
    return expression.arguments.size() == count;
  };

  switch (expression.kind) {
  case ExpressionKind::literal:
    if (!expression.arguments.empty() ||
        !automation_value_valid(expression.literal))
      return fail("invalid literal expression");
    result = value_type(expression.literal);
    return true;
  case ExpressionKind::variable: {
    if (!expression.arguments.empty() || !valid_id(expression.reference))
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
    else if (expression.reference == "value")
      result.reset();
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
    if (!expression.operation.empty() || !require_count(2))
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

bool automation_value_valid_impl(const AutomationValue &value,
                                 const std::size_t depth) noexcept {
  if (depth > max_automation_ast_depth)
    return false;
  if (const auto *number = std::get_if<double>(&value.storage))
    return std::isfinite(*number);
  if (const auto *duration = std::get_if<DurationValue>(&value.storage))
    return duration->value >= Milliseconds::zero() &&
           duration->value <= Milliseconds(86'400'000);
  if (const auto *confidence = std::get_if<ConfidenceValue>(&value.storage))
    return std::isfinite(confidence->value) && confidence->value >= 0.0 &&
           confidence->value <= 1.0;
  if (const auto *string = std::get_if<std::string>(&value.storage))
    return string->size() <= max_automation_string_bytes &&
           detail::is_valid_utf8(*string);
  if (const auto *list = std::get_if<AutomationList>(&value.storage))
    return list->size() <= max_automation_collection_entries &&
           std::ranges::all_of(*list, [&](const auto &child) {
             return automation_value_valid_impl(child, depth + 1U);
           });
  if (const auto *map = std::get_if<AutomationMap>(&value.storage))
    return map->size() <= max_automation_collection_entries &&
           std::ranges::all_of(*map, [&](const auto &entry) {
             return entry.first.size() <= max_automation_string_bytes &&
                    detail::is_valid_utf8(entry.first) &&
                    automation_value_valid_impl(entry.second, depth + 1U);
           });
  return true;
}

} // namespace
AutomationValueType value_type(const AutomationValue &value) noexcept {
  return static_cast<AutomationValueType>(value.storage.index());
}
bool automation_value_valid(const AutomationValue &value) noexcept {
  return automation_value_valid_impl(value, 0);
}
std::string automation_value_json(const AutomationValue &value) {
  std::string output;
  append_value(output, value);
  return output;
}

AutomationValueParseResult
parse_automation_value_json(const std::string_view source,
                            const AutomationValueType expected_type) {
  if (source.size() > max_automation_state_bytes)
    return AutomationError{"$", "automation value exceeds the 64 KiB limit"};
  const auto parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed))
    return AutomationError{"$@" + std::to_string(parse_error->offset),
                           parse_error->message};
  AutomationError error;
  auto value =
      read_value(std::get<Value>(parsed), expected_type, error, "$", 0);
  if (!value.has_value())
    return error;
  return std::move(*value);
}
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

  std::map<std::string, AutomationValueType, std::less<>> variables;
  for (std::size_t index{}; index < profile.variables.size(); ++index) {
    const auto &variable = profile.variables[index];
    const auto path = "$.variables[" + std::to_string(index) + "]";
    if (!valid_id(variable.id) ||
        !variables.emplace(variable.id, variable.type).second)
      return {path + ".id", "duplicate or invalid variable ID"};
    if (variable.type == AutomationValueType::null_value ||
        value_type(variable.default_value) != variable.type ||
        !automation_value_valid(variable.default_value))
      return {path + ".default", "invalid variable default"};
  }

  const auto null_expression = [](const AutomationExpression &expression) {
    return expression.kind == ExpressionKind::literal &&
           expression.arguments.empty() &&
           std::holds_alternative<std::nullptr_t>(expression.literal.storage);
  };
  const auto compatible = [](const std::optional<AutomationValueType> actual,
                             const AutomationValueType expected) {
    if (!actual.has_value())
      return true;
    if (*actual == expected)
      return true;
    return expected == AutomationValueType::number &&
           *actual == AutomationValueType::integer;
  };

  std::set<std::string, std::less<>> automation_ids;
  std::size_t total_rules{};
  for (std::size_t automation_index{};
       automation_index < profile.automations.size(); ++automation_index) {
    const auto &automation = profile.automations[automation_index];
    const auto automation_path =
        "$.automations[" + std::to_string(automation_index) + "]";
    if (!valid_id(automation.id) ||
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
      if (!valid_id(rule.id) || !rule_ids.insert(rule.id).second)
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
        const auto &action = rule.actions[action_index];
        const auto action_path =
            path + ".actions[" + std::to_string(action_index) + "]";
        const auto target_path = action_path + ".target";
        const bool requires_value = action_accepts_value(action.kind);
        const bool has_secondary = !null_expression(action.secondary);
        std::optional<AutomationValueType> value_type_result;
        std::optional<AutomationValueType> secondary_type;
        if (requires_value) {
          if (!validate_expression_tree(action.value, variables,
                                        action_path + ".value", nodes, 1,
                                        value_type_result, expression_error))
            return expression_error;
        } else if (!null_expression(action.value)) {
          return {action_path + ".value",
                  "this action does not accept a value"};
        }
        if (has_secondary &&
            !validate_expression_tree(action.secondary, variables,
                                      action_path + ".secondary", nodes, 1,
                                      secondary_type, expression_error))
          return expression_error;
        if (action.repeating &&
            action.kind != AutomationActionKind::create_timer)
          return {action_path + ".repeating",
                  "only createTimer accepts repeating"};

        const auto variable = variables.find(action.target);
        switch (action.kind) {
        case AutomationActionKind::set_variable:
          if (variable == variables.end())
            return {target_path, "unknown variable ID"};
          if (!compatible(value_type_result, variable->second))
            return {action_path + ".value",
                    "assignment type does not match the variable"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "setVariable does not accept secondary"};
          break;
        case AutomationActionKind::increment_variable:
          if (variable == variables.end())
            return {target_path, "unknown variable ID"};
          if (!numeric_type(variable->second) ||
              !compatible(value_type_result, variable->second))
            return {action_path + ".value",
                    "incrementVariable must preserve the variable type"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "incrementVariable does not accept secondary"};
          break;
        case AutomationActionKind::append_list:
          if (variable == variables.end() ||
              variable->second != AutomationValueType::list)
            return {target_path, "appendList requires a list variable"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "appendList does not accept secondary"};
          break;
        case AutomationActionKind::set_map_entry:
          if (variable == variables.end() ||
              variable->second != AutomationValueType::map)
            return {target_path, "setMapEntry requires a map variable"};
          if (!has_secondary ||
              !compatible(secondary_type, AutomationValueType::string))
            return {action_path + ".secondary",
                    "setMapEntry requires a string key"};
          break;
        case AutomationActionKind::erase_map_entry:
          if (variable == variables.end() ||
              variable->second != AutomationValueType::map)
            return {target_path, "eraseMapEntry requires a map variable"};
          if (!compatible(value_type_result, AutomationValueType::string))
            return {action_path + ".value",
                    "eraseMapEntry requires a string key"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "eraseMapEntry does not accept secondary"};
          break;
        case AutomationActionKind::trigger_cooldown:
        case AutomationActionKind::reset_cooldown:
        case AutomationActionKind::refund_cooldown:
          if (!cooldown_exists(base, action.target))
            return {target_path, "unknown cooldown ID"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "cooldown action does not accept secondary"};
          break;
        case AutomationActionKind::reduce_cooldown_next:
        case AutomationActionKind::reduce_cooldown_all:
          if (!cooldown_exists(base, action.target))
            return {target_path, "unknown cooldown ID"};
          if (value_type_result.has_value() &&
              *value_type_result != AutomationValueType::duration &&
              *value_type_result != AutomationValueType::integer)
            return {action_path + ".value",
                    "cooldown reduction requires a duration"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "cooldown reduction does not accept secondary"};
          break;
        case AutomationActionKind::trigger_group:
        case AutomationActionKind::reset_group:
          if (!group_exists(base, action.target))
            return {target_path, "unknown cooldown group ID"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "group action does not accept secondary"};
          break;
        case AutomationActionKind::create_timer:
          if (!valid_id(action.target))
            return {target_path, "invalid timer ID"};
          if (value_type_result.has_value() &&
              *value_type_result != AutomationValueType::duration &&
              *value_type_result != AutomationValueType::integer)
            return {action_path + ".value", "createTimer requires a duration"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "createTimer does not accept secondary"};
          break;
        case AutomationActionKind::cancel_timer:
          if (!valid_id(action.target))
            return {target_path, "invalid timer ID"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "cancelTimer does not accept secondary"};
          break;
        case AutomationActionKind::set_overlay_override: {
          if (!panel_exists(base, action.target))
            return {target_path, "unknown panel ID"};
          if (!valid_id(action.name))
            return {action_path + ".name",
                    "overlay overrides require a valid name"};
          const auto property_type = overlay_property_type(action.property);
          if (!property_type.has_value())
            return {action_path + ".property", "unsupported overlay property"};
          if (!compatible(value_type_result, *property_type))
            return {action_path + ".value",
                    "overlay value does not match the property type"};
          if (has_secondary && secondary_type.has_value() &&
              *secondary_type != AutomationValueType::duration &&
              *secondary_type != AutomationValueType::integer)
            return {action_path + ".secondary",
                    "overlay expiry must be a duration"};
          break;
        }
        case AutomationActionKind::clear_overlay_override:
          if (!valid_id(action.target))
            return {target_path, "invalid overlay override name"};
          if (has_secondary)
            return {action_path + ".secondary",
                    "clearOverlayOverride does not accept secondary"};
          break;
        }
      }
    }
  }
  return {};
}
AutomationProfileResult parse_automation_profile_json(std::string_view source,
                                                      const GameProfile &base) {
  if (source.size() > max_automation_json_bytes)
    return AutomationError{"$", "automation sidecar exceeds the 256 KiB limit"};
  auto parsed = json::parse(source);
  if (const auto *error = std::get_if<json::ParseError>(&parsed))
    return AutomationError{"$@" + std::to_string(error->offset),
                           error->message};
  return Parser{std::get<Value>(parsed), base, {}}.parse();
}
std::string
serialize_automation_profile_json(const AutomationProfile &profile) {
  std::string out;
  out.reserve(4096);
  out += "{\n  \"schemaVersion\":1,\n  \"profileId\":";
  json::append_quoted(out, profile.profile_id);
  out += ",\n  \"continuousInput\":{\"deadzone\":";
  json::append_number(out, profile.continuous_input.deadzone);
  out += ",\"changeThreshold\":";
  json::append_number(out, profile.continuous_input.change_threshold);
  out += ",\"maxEventsPerSecond\":";
  json::append_unsigned(out, profile.continuous_input.max_events_per_second);
  out += "},\n  \"variables\":[";
  for (std::size_t i{}; i < profile.variables.size(); ++i) {
    if (i)
      out.push_back(',');
    const auto &variable = profile.variables[i];
    out += "\n    {\"id\":";
    json::append_quoted(out, variable.id);
    out += ",\"type\":";
    json::append_quoted(out, automation_value_type_name(variable.type));
    out += ",\"persistent\":";
    out += variable.persistent ? "true" : "false";
    out += ",\"default\":";
    append_value(out, variable.default_value);
    out.push_back('}');
  }
  if (!profile.variables.empty())
    out.push_back('\n');
  out += "  ],\n  \"automations\":[";
  for (std::size_t ai{}; ai < profile.automations.size(); ++ai) {
    if (ai)
      out.push_back(',');
    const auto &automation = profile.automations[ai];
    out += "\n    {\"id\":";
    json::append_quoted(out, automation.id);
    out += ",\"name\":";
    json::append_quoted(out, automation.name);
    out += ",\"enabled\":";
    out += automation.enabled ? "true" : "false";
    out += ",\"rules\":[";
    for (std::size_t ri{}; ri < automation.rules.size(); ++ri) {
      if (ri)
        out.push_back(',');
      const auto &rule = automation.rules[ri];
      out += "{\"id\":";
      json::append_quoted(out, rule.id);
      out += ",\"priority\":";
      if (rule.priority < 0) {
        out.push_back('-');
        json::append_unsigned(out,
                              static_cast<std::uint64_t>(
                                  -static_cast<std::int64_t>(rule.priority)));
      } else
        json::append_unsigned(out, static_cast<std::uint64_t>(rule.priority));
      out += ",\"event\":";
      json::append_quoted(out, automation_event_name(rule.event));
      if (!rule.source_filter.empty()) {
        out += ",\"source\":";
        json::append_quoted(out, rule.source_filter);
      }
      out += ",\"condition\":";
      append_expression(out, rule.condition);
      out += ",\"actions\":[";
      for (std::size_t xi{}; xi < rule.actions.size(); ++xi) {
        if (xi)
          out.push_back(',');
        const auto &action = rule.actions[xi];
        out += "{\"type\":";
        json::append_quoted(out, action_name(action.kind));
        out += ",\"target\":";
        json::append_quoted(out, action.target);
        if (!action.name.empty()) {
          out += ",\"name\":";
          json::append_quoted(out, action.name);
        }
        if (!action.property.empty()) {
          out += ",\"property\":";
          json::append_quoted(out, action.property);
        }
        if (action.repeating)
          out += ",\"repeating\":true";
        if (action_accepts_value(action.kind)) {
          out += ",\"value\":";
          append_expression(out, action.value);
        }
        if (action.secondary.kind != ExpressionKind::literal ||
            !std::holds_alternative<std::nullptr_t>(
                action.secondary.literal.storage)) {
          out += ",\"secondary\":";
          append_expression(out, action.secondary);
        }
        out.push_back('}');
      }
      out += "]}";
    }
    out += "]}";
  }
  if (!profile.automations.empty())
    out.push_back('\n');
  out += "  ]\n}\n";
  return out;
}

AutomationStateResult
parse_automation_state_json(const std::string_view source,
                            const AutomationProfile &profile) {
  if (source.size() > max_automation_state_bytes)
    return AutomationError{"$", "automation state exceeds the 64 KiB limit"};
  auto parsed = json::parse(source);
  if (const auto *error = std::get_if<json::ParseError>(&parsed))
    return AutomationError{"$@" + std::to_string(error->offset),
                           error->message};
  const auto &root = std::get<Value>(parsed);
  if (!root.is_object())
    return AutomationError{"$", "automation state root must be an object"};
  const auto *schema = root.find("schemaVersion");
  const auto *profile_id = root.find("profileId");
  const auto *values = root.find("values");
  if (schema == nullptr || !schema->is_number() || schema->as_number() != 1)
    return AutomationError{"$.schemaVersion",
                           "unsupported automation state schema"};
  if (profile_id == nullptr || !profile_id->is_string() ||
      profile_id->as_string() != profile.profile_id)
    return AutomationError{"$.profileId",
                           "automation state profile ID does not match"};
  if (values == nullptr || !values->is_object())
    return AutomationError{"$.values", "expected object"};

  AutomationPersistentValues result;
  for (const auto &[id, raw] : values->as_object()) {
    const auto declaration =
        std::ranges::find(profile.variables, id, &AutomationVariable::id);
    if (declaration == profile.variables.end() || !declaration->persistent)
      return AutomationError{"$.values." + id,
                             "unknown or non-persistent variable"};
    AutomationError error;
    auto value = read_value(raw, declaration->type, error, "$.values." + id);
    if (!value.has_value())
      return error;
    result.emplace(id, std::move(*value));
  }
  return result;
}

std::string
serialize_automation_state_json(const AutomationProfile &profile,
                                const AutomationPersistentValues &values) {
  std::string output = "{\"schemaVersion\":1,\"profileId\":";
  json::append_quoted(output, profile.profile_id);
  output += ",\"values\":{";
  std::size_t emitted{};
  for (const auto &variable : profile.variables) {
    if (!variable.persistent)
      continue;
    const auto found = values.find(variable.id);
    if (found == values.end() || value_type(found->second) != variable.type ||
        !automation_value_valid(found->second))
      continue;
    if (emitted++ != 0)
      output.push_back(',');
    json::append_quoted(output, variable.id);
    output.push_back(':');
    append_value(output, found->second);
  }
  output += "}}\n";
  return output;
}

} // namespace layerc::core
