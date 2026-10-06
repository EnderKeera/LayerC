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

// Profile literals, defaults, and persisted state use the shared codec with
// $type/$value tags.
void append_value(std::string &out, const AutomationValue &value) {
  detail::append_automation_value(out, value, detail::state_value_tags);
}

std::optional<AutomationValue> read_value(const Value &value,
                                          const AutomationValueType type,
                                          AutomationError &error,
                                          const std::string &path) {
  return detail::read_automation_value(value, type, detail::state_value_tags,
                                       error, path);
}

// Reads the automation document's shape and types only. Every semantic
// bound (IDs, ranges, arity, references) is owned by
// validate_automation_profile, which parse() runs on the complete result. The
// json::Reader is the only error channel.
struct Parser {
  const Value &root;
  const GameProfile &base;
  json::Reader fields{};

  bool fail(std::string path, std::string message) {
    return fields.fail(std::move(path), std::move(message));
  }
  const Value *required(const Value &parent, std::string_view key,
                        const std::string &path) {
    return fields.field(parent, key, path, json::Required::yes);
  }
  bool string(const Value &parent, std::string_view key,
              const std::string &path, std::string &out,
              bool optional = false) {
    return fields.string(parent, key, path, out,
                         optional ? json::Required::no : json::Required::yes,
                         json::EmptyString::allow);
  }
  bool boolean(const Value &parent, std::string_view key,
               const std::string &path, bool &out, bool optional = false) {
    return fields.boolean(parent, key, path, out,
                          optional ? json::Required::no : json::Required::yes);
  }
  const Value::Array *array(const Value &parent, std::string_view key,
                            const std::string &path) {
    return fields.array(parent, key, path, json::Required::yes);
  }
  // Unknown-field policy: like the profile, visual, and companion sidecars,
  // automation rejects unknown fields at every object level. The serializer
  // has only ever emitted the keys accepted here, so no file LayerC wrote is
  // stranded, and a misspelt optional field (for example "priorty") fails
  // loudly instead of silently falling back to its default.
  bool known(const Value &value, std::initializer_list<std::string_view> allowed,
             const std::string &path) {
    return fields.exact_fields(value, allowed, path);
  }
  bool value(const Value &raw, const AutomationValueType type,
             const std::string &path, AutomationValue &out) {
    AutomationError error;
    auto parsed = read_value(raw, type, error, path);
    if (!parsed)
      return fail(std::move(error.path), std::move(error.message));
    out = std::move(*parsed);
    return true;
  }
  bool continuous_input(const Value &input, ContinuousInputSettings &out) {
    const std::string path = "$.continuousInput";
    if (!input.is_object())
      return fail(path, "expected object");
    if (!known(input, {"deadzone", "changeThreshold", "maxEventsPerSecond"},
               path))
      return false;
    if (const auto *raw = input.find("deadzone")) {
      if (!raw->is_number())
        return fail(path + ".deadzone", "expected 0 through 0.95");
      out.deadzone = raw->as_number();
    }
    if (const auto *raw = input.find("changeThreshold")) {
      if (!raw->is_number())
        return fail(path + ".changeThreshold", "expected above 0 through 1");
      out.change_threshold = raw->as_number();
    }
    if (const auto *raw = input.find("maxEventsPerSecond")) {
      const auto events = json::read_integer(*raw);
      if (!events)
        return fail(path + ".maxEventsPerSecond",
                    "expected integer 1 through 240");
      // Out-of-range integers become 0, which the validator rejects with the
      // same message.
      out.max_events_per_second =
          *events < 0 || *events > std::numeric_limits<std::uint32_t>::max()
              ? 0U
              : static_cast<std::uint32_t>(*events);
    }
    return true;
  }
  bool expression(const Value &raw, AutomationExpression &out,
                  const std::string &path) {
    out = AutomationExpression{};

    if (!raw.is_object())
      return fail(path, "expression must be an object");
    std::string op;
    if (!string(raw, "op", path, op))
      return false;
    if (op == "literal") {
      out.kind = ExpressionKind::literal;
      if (!known(raw, {"op", "valueType", "value"}, path))
        return false;
      std::string declared;
      if (!string(raw, "valueType", path, declared))
        return false;
      const auto type = detail::parse_automation_value_type_name(declared);
      const auto *literal = required(raw, "value", path);
      if (!type || !literal)
        return fail(path, "literal requires a supported valueType and value");
      return value(*literal, *type, path + ".value", out.literal);
    }
    if (op == "variable" || op == "event") {
      out.kind = op == "variable" ? ExpressionKind::variable
                                  : ExpressionKind::event_field;
      const std::string_view reference = op == "variable" ? "id" : "field";
      return known(raw, {"op", reference}, path) &&
             string(raw, reference, path, out.reference);
    }
    if (op == "index") {
      out.kind = ExpressionKind::index;
      if (!known(raw, {"op", "target", "key"}, path))
        return false;
      const auto *target = required(raw, "target", path);
      const auto *key = required(raw, "key", path);
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
    if (!known(raw, {"op", "args"}, path))
      return false;
    const auto *args = array(raw, "args", path);
    if (!args)
      return false;
    out.arguments.resize(args->size());
    for (std::size_t i{}; i < out.arguments.size(); ++i)
      if (!expression((*args)[i], out.arguments[i],
                      path + ".args[" + std::to_string(i) + "]"))
        return false;
    return true;
  }
  bool action(const Value &raw, AutomationAction &out,
              const std::string &path) {
    if (!raw.is_object())
      return fail(path, "action must be an object");
    if (!known(raw,
               {"type", "target", "name", "property", "repeating", "value",
                "secondary"},
               path))
      return false;
    std::string kind;
    if (!string(raw, "type", path, kind))
      return false;
    const auto parsed = parse_automation_action_name(kind);
    if (!parsed)
      return fail(path + ".type", "unsupported automation action");
    out.kind = *parsed;
    if (!string(raw, "target", path, out.target) ||
        !string(raw, "name", path, out.name, true) ||
        !string(raw, "property", path, out.property, true) ||
        !boolean(raw, "repeating", path, out.repeating, true))
      return false;
    if (const auto *value = raw.find("value")) {
      out.value.emplace();
      if (!expression(*value, *out.value, path + ".value"))
        return false;
    }
    if (const auto *secondary = raw.find("secondary")) {
      out.secondary.emplace();
      if (!expression(*secondary, *out.secondary, path + ".secondary"))
        return false;
    }
    return true;
  }
  bool read_variable(const Value &entry, const std::string &path,
                     AutomationVariable &variable) {
    if (!entry.is_object())
      return fail(path, "variable must be an object");
    std::string declared;
    if (!known(entry, {"id", "type", "persistent", "default"}, path) ||
        !string(entry, "id", path, variable.id) ||
        !string(entry, "type", path, declared) ||
        !boolean(entry, "persistent", path, variable.persistent, true))
      return false;
    const auto type = detail::parse_automation_value_type_name(declared);
    if (!type)
      return fail(path + ".type", "variables require a non-null supported type");
    variable.type = *type;
    const auto *raw = required(entry, "default", path);
    return raw != nullptr &&
           value(*raw, variable.type, path + ".default", variable.default_value);
  }
  bool read_rule(const Value &entry, const std::string &path,
                 AutomationRule &rule) {
    if (!entry.is_object())
      return fail(path, "rule must be an object");
    std::string event;
    if (!known(entry,
               {"id", "priority", "event", "source", "condition", "actions"},
               path) ||
        !string(entry, "id", path, rule.id) ||
        !string(entry, "event", path, event) ||
        !string(entry, "source", path, rule.source_filter, true))
      return false;
    const auto parsed_event = parse_automation_event_name(event);
    if (!parsed_event)
      return fail(path + ".event", "unsupported event source");
    rule.event = *parsed_event;
    if (const auto *raw = entry.find("priority")) {
      // Shape only: the value must be an int32; its bound is the validator's.
      const auto priority = json::read_integer(*raw);
      if (!priority || *priority < std::numeric_limits<std::int32_t>::min() ||
          *priority > std::numeric_limits<std::int32_t>::max())
        return fail(path + ".priority", "priority must be a bounded integer");
      rule.priority = static_cast<std::int32_t>(*priority);
    }
    if (const auto *condition = entry.find("condition"))
      if (!expression(*condition, rule.condition, path + ".condition"))
        return false;
    const auto *actions = array(entry, "actions", path);
    if (!actions)
      return false;
    rule.actions.resize(actions->size());
    for (std::size_t i{}; i < rule.actions.size(); ++i)
      if (!action((*actions)[i], rule.actions[i],
                  path + ".actions[" + std::to_string(i) + "]"))
        return false;
    return true;
  }
  bool read_automation(const Value &entry, const std::string &path,
                       AutomationDefinition &automation) {
    if (!entry.is_object())
      return fail(path, "automation must be an object");
    if (!known(entry, {"id", "name", "enabled", "rules"}, path) ||
        !string(entry, "id", path, automation.id) ||
        !string(entry, "name", path, automation.name) ||
        !boolean(entry, "enabled", path, automation.enabled, true))
      return false;
    const auto *rules = array(entry, "rules", path);
    if (!rules)
      return false;
    automation.rules.resize(rules->size());
    for (std::size_t i{}; i < rules->size(); ++i)
      if (!read_rule((*rules)[i], path + ".rules[" + std::to_string(i) + "]",
                     automation.rules[i]))
        return false;
    return true;
  }
  bool read(AutomationProfile &profile) {
    if (!root.is_object())
      return fail("$", "automation root must be an object");
    // The schema version selects the reader, so it is checked before the
    // remaining fields; detail::automation_header_error owns the rule.
    const auto *schema = root.find("schemaVersion");
    const auto version = schema ? json::read_integer(*schema) : std::nullopt;
    profile.schema_version =
        version && *version >= 0 &&
                *version <= std::numeric_limits<std::uint32_t>::max()
            ? static_cast<std::uint32_t>(*version)
            : 0U;
    if (auto header = detail::automation_header_error(profile, nullptr);
        !header.message.empty())
      return fail(std::move(header.path), std::move(header.message));
    if (!known(root,
               {"schemaVersion", "profileId", "continuousInput", "variables",
                "automations"},
               "$") ||
        !string(root, "profileId", "$", profile.profile_id))
      return false;
    if (auto header = detail::automation_header_error(profile, &base);
        !header.message.empty())
      return fail(std::move(header.path), std::move(header.message));
    if (const auto *input = root.find("continuousInput"))
      if (!continuous_input(*input, profile.continuous_input))
        return false;
    const auto *variables = array(root, "variables", "$");
    const auto *automations = variables ? array(root, "automations", "$") : nullptr;
    if (!automations)
      return false;
    profile.variables.resize(variables->size());
    for (std::size_t i{}; i < variables->size(); ++i)
      if (!read_variable((*variables)[i],
                         "$.variables[" + std::to_string(i) + "]",
                         profile.variables[i]))
        return false;
    profile.automations.resize(automations->size());
    for (std::size_t i{}; i < automations->size(); ++i)
      if (!read_automation((*automations)[i],
                           "$.automations[" + std::to_string(i) + "]",
                           profile.automations[i]))
        return false;
    return true;
  }
  AutomationProfileResult parse() {
    AutomationProfile profile;
    if (!read(profile))
      return fields.error();
    if (auto validation = validate_automation_profile(profile, base);
        !validation.message.empty())
      return validation;
    return profile;
  }
};

Value expression_tree(const AutomationExpression &expression) {
  if (expression.kind == ExpressionKind::literal)
    return Value(Value::Object{
        {"op", Value(std::string("literal"))},
        {"valueType", Value(std::string(automation_value_type_name(value_type(expression.literal))))},
        {"value", detail::automation_value_tree(expression.literal)}});
  if (expression.kind == ExpressionKind::variable ||
      expression.kind == ExpressionKind::event_field) {
    const bool variable = expression.kind == ExpressionKind::variable;
    return Value(Value::Object{
        {"op", Value(std::string(variable ? "variable" : "event"))},
        {variable ? "id" : "field", Value(expression.reference)}});
  }
  if (expression.kind == ExpressionKind::index)
    return Value(Value::Object{
        {"op", Value(std::string("index"))},
        {"target", expression_tree(expression.arguments[0])},
        {"key", expression_tree(expression.arguments[1])}});
  Value::Array arguments;
  for (const auto &argument : expression.arguments)
    arguments.push_back(expression_tree(argument));
  return Value(Value::Object{
      {"op", Value(expression.operation)}, {"args", Value(std::move(arguments))}});
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
bool detail::automation_value_valid_at_depth(
    const AutomationValue &value, const std::size_t depth) noexcept {
  return automation_value_valid_impl(value, depth);
}
bool automation_value_valid(const AutomationValue &value) noexcept {
  return detail::automation_value_valid_at_depth(value, 0);
}
bool automation_persistent_value_accepted(const AutomationVariable &variable,
                                          const AutomationValue &value) noexcept {
  return variable.persistent && value_type(value) == variable.type &&
         automation_value_valid(value);
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
  return Parser{root, base}.parse();
}
std::string serialize_automation_profile_json(const AutomationProfile &profile) {
  Value::Array variables;
  for (const auto &variable : profile.variables)
    variables.emplace_back(Value::Object{
        {"id", Value(variable.id)},
        {"type", Value(std::string(automation_value_type_name(variable.type)))},
        {"persistent", Value(variable.persistent)},
        {"default", detail::automation_value_tree(variable.default_value)}});
  Value::Array automations;
  for (const auto &automation : profile.automations) {
    Value::Array rules;
    for (const auto &rule : automation.rules) {
      Value::Array actions;
      for (const auto &action : rule.actions) {
        Value::Object object{
            {"type", Value(std::string(automation_action_name(action.kind).value_or(std::string_view{})))},
            {"target", Value(action.target)}};
        if (!action.name.empty()) object.emplace("name", Value(action.name));
        if (!action.property.empty()) object.emplace("property", Value(action.property));
        if (action.repeating) object.emplace("repeating", Value(true));
        if (action.value) object.emplace("value", expression_tree(*action.value));
        if (action.secondary) object.emplace("secondary", expression_tree(*action.secondary));
        actions.emplace_back(std::move(object));
      }
      Value::Object object{
          {"id", Value(rule.id)}, {"priority", json::exact_integer_value(rule.priority)},
          {"event", Value(std::string(automation_event_name(rule.event)))},
          {"condition", expression_tree(rule.condition)},
          {"actions", Value(std::move(actions))}};
      if (!rule.source_filter.empty()) object.emplace("source", Value(rule.source_filter));
      rules.emplace_back(std::move(object));
    }
    automations.emplace_back(Value::Object{
        {"id", Value(automation.id)}, {"name", Value(automation.name)},
        {"enabled", Value(automation.enabled)}, {"rules", Value(std::move(rules))}});
  }
  Value root(Value::Object{
      {"schemaVersion", Value(static_cast<std::int64_t>(profile.schema_version))},
      {"profileId", Value(profile.profile_id)},
      {"continuousInput", Value(Value::Object{
          {"deadzone", Value(profile.continuous_input.deadzone)},
          {"changeThreshold", Value(profile.continuous_input.change_threshold)},
          {"maxEventsPerSecond", Value(static_cast<std::int64_t>(profile.continuous_input.max_events_per_second))}})},
      {"variables", Value(std::move(variables))},
      {"automations", Value(std::move(automations))}});
  std::string output;
  json::append_value(output, root);
  output.push_back('\n');
  return output;
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

std::string serialize_automation_state_json(
    const AutomationProfile &profile, const AutomationPersistentValues &values) {
  Value::Object saved;
  for (const auto &variable : profile.variables) {
    const auto found = values.find(variable.id);
    if (found != values.end() &&
        automation_persistent_value_accepted(variable, found->second))
      saved.emplace(variable.id, detail::automation_value_tree(found->second));
  }
  std::string output;
  json::append_value(output, Value(Value::Object{
      {"schemaVersion", Value(std::int64_t{1})}, {"profileId", Value(profile.profile_id)},
      {"values", Value(std::move(saved))}}));
  output.push_back('\n');
  return output;
}

} // namespace layerc::core
