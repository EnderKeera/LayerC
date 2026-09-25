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
          json::Join join;
          for (const auto &child : entry) {
            join(out);
            append_tagged_value(out, child);
          }
          out.push_back(']');
        } else if constexpr (std::is_same_v<T, AutomationMap>) {
          out.push_back('{');
          json::Join join;
          for (const auto &[key, child] : entry) {
            join(out);
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
      const auto type =
          detail::parse_automation_value_type_name(tag->as_string());
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
  // Unknown-field policy: like the profile, visual, and companion sidecars,
  // automation rejects unknown fields at every object level. The serializer
  // has only ever emitted the keys accepted here, so no file LayerC wrote is
  // stranded, and a misspelt optional field (for example "priorty") fails
  // loudly instead of silently falling back to its default.
  bool known(const Value &value, std::initializer_list<std::string_view> allowed,
             const std::string &path) {
    return fields.exact_fields(value, allowed, path) ||
           fail(fields.error().path, fields.error().message);
  }
  bool continuous_input(const Value &input, ContinuousInputSettings &out) {
    const std::string path = "$.continuousInput";
    if (!input.is_object())
      return fail(path, "expected object");
    if (!known(input, {"deadzone", "changeThreshold", "maxEventsPerSecond"},
               path))
      return false;
    if (const auto *raw = input.find("deadzone")) {
      if (!raw->is_number() || raw->as_number() < 0 || raw->as_number() > .95)
        return fail(path + ".deadzone", "expected 0 through 0.95");
      out.deadzone = raw->as_number();
    }
    if (const auto *raw = input.find("changeThreshold")) {
      if (!raw->is_number() || raw->as_number() <= 0 || raw->as_number() > 1)
        return fail(path + ".changeThreshold", "expected above 0 through 1");
      out.change_threshold = raw->as_number();
    }
    if (const auto *raw = input.find("maxEventsPerSecond")) {
      const auto events = json::read_integer(*raw);
      if (!events || *events < 1 || *events > 240)
        return fail(path + ".maxEventsPerSecond",
                    "expected integer 1 through 240");
      out.max_events_per_second = static_cast<std::uint32_t>(*events);
    }
    return true;
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
      if (!known(value, {"op", "valueType", "value"}, path))
        return false;
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
      const std::string_view reference = op == "variable" ? "id" : "field";
      return known(value, {"op", reference}, path) &&
             string(value, reference, path, out.reference);
    }
    if (op == "index") {
      out.kind = ExpressionKind::index;
      if (!known(value, {"op", "target", "key"}, path))
        return false;
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
    if (!known(value, {"op", "args"}, path))
      return false;
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
    if (!known(value,
               {"type", "target", "name", "property", "repeating", "value",
                "secondary"},
               path))
      return false;
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
      out.value.emplace();
      if (!expression(*raw, *out.value, path + ".value"))
        return false;
    }
    if (const auto *raw = value.find("secondary")) {
      out.secondary.emplace();
      if (!expression(*raw, *out.secondary, path + ".secondary"))
        return false;
    }
    return true;
  }
  // The field reader's error wins when an id is not a string; otherwise the
  // id failed valid_automation_id and the domain message is recorded.
  bool read_id(const Value &entry, const std::string &path, std::string &id,
               std::set<std::string, std::less<>> &seen,
               const std::string_view kind) {
    if (!string(entry, "id", path, id) || !detail::valid_automation_id(id))
      return fail(path + ".id", "invalid " + std::string(kind) + " ID");
    if (!seen.insert(id).second)
      return fail(path + ".id", "duplicate " + std::string(kind) + " ID");
    return true;
  }
  bool read_variable(const Value &entry, const std::string &path,
                     std::set<std::string, std::less<>> &ids,
                     AutomationVariable &variable) {
    if (!entry.is_object())
      return fail(path, "variable must be an object");
    if (!known(entry, {"id", "type", "persistent", "default"}, path) ||
        !read_id(entry, path, variable.id, ids, "variable"))
      return false;
    std::string declared;
    if (!string(entry, "type", path, declared) ||
        !boolean(entry, "persistent", path, variable.persistent, true))
      return false;
    const auto type = detail::parse_automation_value_type_name(declared);
    if (!type || *type == AutomationValueType::null_value)
      return fail(path + ".type", "variables require a non-null supported type");
    variable.type = *type;
    const auto *raw = required(entry, "default", path);
    if (!raw)
      return false;
    auto parsed = read_value(*raw, variable.type, error, path + ".default");
    if (!parsed)
      return false;
    variable.default_value = std::move(*parsed);
    return true;
  }
  bool read_rule(const Value &entry, const std::string &path,
                 std::set<std::string, std::less<>> &ids, AutomationRule &rule) {
    if (!entry.is_object())
      return fail(path, "rule must be an object");
    if (!known(entry,
               {"id", "priority", "event", "source", "condition", "actions"},
               path) ||
        !read_id(entry, path, rule.id, ids, "rule"))
      return false;
    std::string event;
    if (!string(entry, "event", path, event) ||
        !string(entry, "source", path, rule.source_filter, true))
      return false;
    const auto parsed_event = parse_automation_event_name(event);
    if (!parsed_event)
      return fail(path + ".event", "unsupported event source");
    rule.event = *parsed_event;
    if (const auto *raw = entry.find("priority")) {
      const auto priority = json::read_integer(*raw);
      if (!priority || *priority < -1'000'000 || *priority > 1'000'000)
        return fail(path + ".priority", "priority must be a bounded integer");
      rule.priority = static_cast<std::int32_t>(*priority);
    }
    if (const auto *condition = entry.find("condition"))
      if (!expression(*condition, rule.condition, path + ".condition"))
        return false;
    const auto *actions = entry.find("actions");
    if (actions == nullptr || !actions->is_array())
      return fail(path + ".actions", "expected array");
    rule.actions.resize(actions->as_array().size());
    for (std::size_t i{}; i < rule.actions.size(); ++i)
      if (!action(actions->as_array()[i], rule.actions[i],
                  path + ".actions[" + std::to_string(i) + "]"))
        return false;
    return true;
  }
  bool read_automation(const Value &entry, const std::string &path,
                       std::set<std::string, std::less<>> &ids,
                       AutomationDefinition &automation) {
    if (!entry.is_object())
      return fail(path, "automation must be an object");
    if (!known(entry, {"id", "name", "enabled", "rules"}, path) ||
        !read_id(entry, path, automation.id, ids, "automation"))
      return false;
    if (!string(entry, "name", path, automation.name) ||
        !boolean(entry, "enabled", path, automation.enabled, true))
      return false;
    const auto *rules = entry.find("rules");
    if (rules == nullptr || !rules->is_array())
      return fail(path + ".rules", "expected array");
    std::set<std::string, std::less<>> rule_ids;
    for (std::size_t i{}; i < rules->as_array().size(); ++i) {
      AutomationRule rule;
      if (!read_rule(rules->as_array()[i],
                     path + ".rules[" + std::to_string(i) + "]", rule_ids, rule))
        return false;
      automation.rules.push_back(std::move(rule));
    }
    return true;
  }
  AutomationProfileResult parse() {
    if (!root.is_object())
      return AutomationError{"$", "automation root must be an object"};
    std::optional<std::int64_t> version;
    if (const auto *schema = root.find("schemaVersion"))
      version = json::read_integer(*schema);
    if (!version || *version != 1)
      return AutomationError{"$.schemaVersion",
                             "unsupported automation schema version"};
    AutomationProfile profile;
    if (!known(root,
               {"schemaVersion", "profileId", "continuousInput", "variables",
                "automations"},
               "$") ||
        !string(root, "profileId", "$", profile.profile_id))
      return error;
    if (profile.profile_id != base.id)
      return AutomationError{
          "$.profileId", "automation profile ID does not match base profile"};
    if (const auto *input = root.find("continuousInput"))
      if (!continuous_input(*input, profile.continuous_input))
        return error;
    const auto *variables = root.find("variables");
    const auto *automations = root.find("automations");
    if (variables == nullptr || !variables->is_array())
      return AutomationError{"$.variables", "expected array"};
    if (automations == nullptr || !automations->is_array())
      return AutomationError{"$.automations", "expected array"};
    std::set<std::string, std::less<>> variable_ids;
    for (std::size_t i{}; i < variables->as_array().size(); ++i) {
      AutomationVariable variable;
      if (!read_variable(variables->as_array()[i],
                         "$.variables[" + std::to_string(i) + "]",
                         variable_ids, variable))
        return error;
      profile.variables.push_back(std::move(variable));
    }
    std::set<std::string, std::less<>> automation_ids;
    for (std::size_t i{}; i < automations->as_array().size(); ++i) {
      AutomationDefinition automation;
      if (!read_automation(automations->as_array()[i],
                           "$.automations[" + std::to_string(i) + "]",
                           automation_ids, automation))
        return error;
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
    json::Join join;
    for (const auto &argument : expression.arguments) {
      join(out);
      append_expression(out, argument);
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
           duration->value <= max_cooldown_duration;
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
  const auto parsed = json::parse_bounded(source, max_automation_state_bytes);
  if (const auto *parse_error = std::get_if<json::FieldError>(&parsed))
    return detail::json_parse_error(*parse_error);
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
  auto parsed = json::parse_bounded(source, max_automation_json_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return detail::json_parse_error(*error);
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
  json::Join join_variables;
  for (const auto &variable : profile.variables) {
    join_variables(out);
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
  json::Join join_automations;
  for (const auto &automation : profile.automations) {
    join_automations(out);
    out += "\n    {\"id\":";
    json::append_quoted(out, automation.id);
    out += ",\"name\":";
    json::append_quoted(out, automation.name);
    out += ",\"enabled\":";
    json::append_bool(out, automation.enabled);
    out += ",\"rules\":[";
    json::Join join_rules;
    for (const auto &rule : automation.rules) {
      join_rules(out);
      out += "{\"id\":";
      json::append_quoted(out, rule.id);
      out += ",\"priority\":";
      detail::append_exact_integer(out, rule.priority);
      out += ",\"event\":";
      json::append_quoted(out, automation_event_name(rule.event));
      if (!rule.source_filter.empty()) {
        out += ",\"source\":";
        json::append_quoted(out, rule.source_filter);
      }
      out += ",\"condition\":";
      append_expression(out, rule.condition);
      out += ",\"actions\":[";
      json::Join join_actions;
      for (const auto &action : rule.actions) {
        join_actions(out);
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
  auto parsed = json::parse_bounded(source, max_automation_state_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return detail::json_parse_error(*error);
  const auto &root = std::get<Value>(parsed);
  if (!root.is_object())
    return AutomationError{"$", "automation state root must be an object"};
  const auto *schema = root.find("schemaVersion");
  const auto *profile_id = root.find("profileId");
  const auto *values = root.find("values");
  const auto state_version =
      schema == nullptr ? std::nullopt : json::read_integer(*schema);
  if (!state_version || *state_version != 1)
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
  json::Join join_values;
  for (const auto &variable : profile.variables) {
    if (!variable.persistent)
      continue;
    const auto found = values.find(variable.id);
    if (found == values.end() || value_type(found->second) != variable.type ||
        !automation_value_valid(found->second))
      continue;
    join_values(output);
    json::append_quoted(output, variable.id);
    output.push_back(':');
    append_value(output, found->second);
  }
  output += "}}\n";
  return output;
}

} // namespace layerc::core
