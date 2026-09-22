#include "layerc/core/automation.hpp"

#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "automation_profile_internal.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/utf8.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <type_traits>
#include <utility>

namespace layerc::core {
namespace {
using detail::automation_event_name;
using detail::automation_value_type_name;
using detail::numeric_type;
using detail::parse_automation_event_name;
using json::Value;

void append_tagged_value(std::string &out, const AutomationValue &value);

void append_value(std::string &out, const AutomationValue &value) {
  std::visit(
      [&](const auto &entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>)
          out += "null";
        else if constexpr (std::is_same_v<T, bool>)
          json::append_bool(out, entry);
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
           const std::string &path);

std::optional<AutomationValue> read_nested_value(const Value &value,
                                                 AutomationError &error,
                                                 const std::string &path) {
  if (value.is_object() && value.as_object().size() == 2U) {
    const auto *tag = value.find("$type");
    const auto *payload = value.find("$value");
    if (tag != nullptr && tag->is_string() && payload != nullptr) {
  const auto type = detail::parse_automation_value_type_name(tag->as_string());
      if (!type.has_value()) {
        error = {path + ".$type", "unsupported nested value type"};
        return std::nullopt;
      }
      return read_value(*payload, *type, error, path + ".$value");
    }
  }
  return read_value(value, infer_type(value), error, path);
}

std::optional<AutomationValue> read_value(const Value &value,
                                           const AutomationValueType type,
                                           AutomationError &error,
                                           const std::string &path) {
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
    if (const auto integer = json::read_integer(
            value, json::IntegerEncoding::number_or_string))
      return AutomationValue(DurationValue{Milliseconds(*integer)});
    break;
  case AutomationValueType::confidence:
    if (value.is_number() && std::isfinite(value.as_number()))
      return AutomationValue(ConfidenceValue{value.as_number()});
    break;
  case AutomationValueType::string:
    if (value.is_string())
      return AutomationValue(value.as_string());
    break;
  case AutomationValueType::list:
    if (value.is_array()) {
      AutomationList result;
      result.reserve(value.as_array().size());
      for (std::size_t i{}; i < value.as_array().size(); ++i) {
        auto child =
            read_nested_value(value.as_array()[i], error,
                              path + "[" + std::to_string(i) + "]");
        if (!child)
          return std::nullopt;
        result.push_back(std::move(*child));
      }
      return AutomationValue(std::move(result));
    }
    break;
  case AutomationValueType::map:
    if (value.is_object()) {
      AutomationMap result;
      for (const auto &[key, raw] : value.as_object()) {
        auto child = read_nested_value(raw, error, path + "." + key);
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
  json::Reader fields{};
  bool fail(std::string path, std::string message) {
    if (error.message.empty())
      error = {std::move(path), std::move(message)};
    return false;
  }
  const Value *required(const Value &parent, std::string_view key,
                        const std::string &path) {
    const auto *value = fields.field(parent, key, path, json::Required::yes);
    if (!value)
      fail(fields.error().path, fields.error().message);
    return value;
  }
  bool string(const Value &parent, std::string_view key,
              const std::string &path, std::string &out,
              bool optional = false) {
    return fields.string(parent, key, path, out,
                         optional ? json::Required::no : json::Required::yes,
                         json::EmptyString::allow) ||
           fail(fields.error().path, fields.error().message);
  }
  bool boolean(const Value &parent, std::string_view key,
               const std::string &path, bool &out, bool optional = false) {
    return fields.boolean(parent, key, path, out,
                          optional ? json::Required::no : json::Required::yes) ||
           fail(fields.error().path, fields.error().message);
  }
  bool expression(const Value &value, AutomationExpression &out,
                  const std::string &path) {
    out = AutomationExpression{};

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
    const auto type = detail::parse_automation_value_type_name(declared);
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
      return expression(*target, out.arguments[0], path + ".target") &&
             expression(*key, out.arguments[1], path + ".key");
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
                      path + ".args[" + std::to_string(i) + "]"))
        return false;
    return true;
  }
  bool action(const Value &value, AutomationAction &out,
              const std::string &path) {
    if (!value.is_object())
      return fail(path, "action must be an object");
    std::string kind;
    if (!string(value, "type", path, kind))
      return false;
    const auto parsed = parse_automation_action_name(kind);
    if (!parsed)
      return fail(path + ".type", "unsupported automation action");
    out.kind = *parsed;
    if (!string(value, "target", path, out.target) ||
        !string(value, "name", path, out.name, true) ||
        !string(value, "property", path, out.property, true) ||
        !boolean(value, "repeating", path, out.repeating, true))
      return false;
    if (const auto *raw = value.find("value")) {
      AutomationExpression parsed_value;
      if (!expression(*raw, parsed_value, path + ".value"))
        return false;
      out.value = std::move(parsed_value);
    }
    if (const auto *raw = value.find("secondary")) {
      AutomationExpression parsed_secondary;
      if (!expression(*raw, parsed_secondary, path + ".secondary"))
        return false;
      out.secondary = std::move(parsed_secondary);
    }
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
    std::set<std::string, std::less<>> variable_ids;
    for (std::size_t i{}; i < variables->as_array().size(); ++i) {
      const auto path = "$.variables[" + std::to_string(i) + "]";
      const auto &entry = variables->as_array()[i];
      if (!entry.is_object())
        return AutomationError{path, "variable must be an object"};
      AutomationVariable variable;
      std::string declared;
      if (!string(entry, "id", path, variable.id) ||
          !detail::valid_automation_id(variable.id))
        return error.message.empty()
                   ? AutomationError{path + ".id", "invalid variable ID"}
                   : error;
      if (!variable_ids.insert(variable.id).second)
        return AutomationError{path + ".id", "duplicate variable ID"};
      if (!string(entry, "type", path, declared) ||
          !boolean(entry, "persistent", path, variable.persistent, true))
        return error;
    const auto type = detail::parse_automation_value_type_name(declared);
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
    for (std::size_t ai{}; ai < automations->as_array().size(); ++ai) {
      const auto path = "$.automations[" + std::to_string(ai) + "]";
      const auto &entry = automations->as_array()[ai];
      if (!entry.is_object())
        return AutomationError{path, "automation must be an object"};
      AutomationDefinition automation;
      if (!string(entry, "id", path, automation.id) ||
          !detail::valid_automation_id(automation.id))
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
      std::set<std::string, std::less<>> rule_ids;
      for (std::size_t ri{}; ri < rules->as_array().size(); ++ri) {
        const auto rule_path = path + ".rules[" + std::to_string(ri) + "]";
        const auto &entry_rule = rules->as_array()[ri];
        if (!entry_rule.is_object())
          return AutomationError{rule_path, "rule must be an object"};
        AutomationRule rule;
        std::string event;
        if (!string(entry_rule, "id", rule_path, rule.id) ||
            !detail::valid_automation_id(rule.id))
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
        if (const auto *condition = entry_rule.find("condition"))
          if (!expression(*condition, rule.condition, rule_path + ".condition"))
            return error;
        const auto *actions = required(entry_rule, "actions", rule_path);
        if (!actions || !actions->is_array())
          return AutomationError{rule_path + ".actions", "expected array"};
        rule.actions.resize(actions->as_array().size());
        for (std::size_t i{}; i < rule.actions.size(); ++i)
          if (!action(actions->as_array()[i], rule.actions[i],
                      rule_path + ".actions[" + std::to_string(i) + "]"))
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
    json::append_quoted(
        out, automation_value_type_name(value_type(expression.literal)));
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
           is_valid_utf8(*string);
  if (const auto *list = std::get_if<AutomationList>(&value.storage))
    return list->size() <= max_automation_collection_entries &&
           std::ranges::all_of(*list, [&](const auto &child) {
             return automation_value_valid_impl(child, depth + 1U);
           });
  if (const auto *map = std::get_if<AutomationMap>(&value.storage))
    return map->size() <= max_automation_collection_entries &&
           std::ranges::all_of(*map, [&](const auto &entry) {
             return entry.first.size() <= max_automation_string_bytes &&
                    is_valid_utf8(entry.first) &&
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
  auto value = read_value(std::get<Value>(parsed), expected_type, error, "$");
  if (!value.has_value())
    return error;
  return std::move(*value);
}
AutomationProfileResult parse_automation_profile_json(std::string_view source,
                                                      const GameProfile &base) {
  if (source.size() > max_automation_json_bytes)
    return AutomationError{"$", "automation sidecar exceeds the 256 KiB limit"};
  auto parsed = json::parse(source);
  if (const auto *error = std::get_if<json::ParseError>(&parsed))
    return AutomationError{"$@" + std::to_string(error->offset),
                           error->message};
  return parse_automation_profile_value(std::get<Value>(parsed), base);
}
AutomationProfileResult parse_automation_profile_value(const json::Value &root,
                                                       const GameProfile &base) {
  return Parser{root, base, {}}.parse();
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
    json::append_bool(out, variable.persistent);
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
    json::append_bool(out, automation.enabled);
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
        json::append_quoted(
            out, automation_action_name(action.kind).value_or(std::string_view{}));
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
        if (action.value.has_value()) {
          out += ",\"value\":";
          append_expression(out, *action.value);
        }
        if (action.secondary.has_value()) {
          out += ",\"secondary\":";
          append_expression(out, *action.secondary);
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
