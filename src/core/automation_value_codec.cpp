#include "automation_json.hpp"

#include "layerc/core/json.hpp"

#include <cmath>
#include <type_traits>
#include <utility>

// The one AutomationValue JSON codec. Profile literals, persisted state, and
// traces differ only in their tag keys. Decoding reads shape and type only;
// the bounded entry points then apply automation_value_valid, the single
// bounds predicate. The profile reader uses the shape-only entry point because
// validate_automation_profile owns its semantic bounds. The JSON parser bounds
// nesting.
namespace layerc::core::detail {
namespace {

struct TaggedPayload {
  const json::Value *payload;
  AutomationValueType type;
  std::string path;
};

// The {type-tag, value-tag} envelope of a value whose type the reader does not
// know. Shape only: the payload is decoded by the caller.
std::optional<TaggedPayload> tagged_payload(const json::Value &value,
                                            const AutomationValueTags tags,
                                            AutomationError &error,
                                            const std::string &path) {
  const auto *tag = value.is_object() ? value.find(tags.type) : nullptr;
  const auto *payload = value.is_object() ? value.find(tags.value) : nullptr;
  if (tag == nullptr || !tag->is_string() || payload == nullptr ||
      value.as_object().size() != 2U) {
    error = {path, "value requires exactly " + std::string(tags.type) +
                       " and " + std::string(tags.value)};
    return std::nullopt;
  }
  const auto type = parse_automation_value_type_name(tag->as_string());
  if (!type) {
    error = {path + "." + std::string(tags.type), "unsupported value type"};
    return std::nullopt;
  }
  return TaggedPayload{payload, *type, path + "." + std::string(tags.value)};
}

[[nodiscard]] AutomationValueType infer_type(const json::Value &value) {
  if (value.is_null()) return AutomationValueType::null_value;
  if (value.is_bool()) return AutomationValueType::boolean;
  if (value.is_number()) return AutomationValueType::number;
  if (value.is_string()) return AutomationValueType::string;
  if (value.is_array()) return AutomationValueType::list;
  return AutomationValueType::map;
}

std::optional<AutomationValue> read_shape(const json::Value &value,
                                          AutomationValueType type,
                                          AutomationValueTags tags,
                                          AutomationError &error,
                                          const std::string &path);

// A list or map child: a tagged value, or plain JSON of its inferred type.
std::optional<AutomationValue> read_child(const json::Value &value,
                                          const AutomationValueTags tags,
                                          AutomationError &error,
                                          const std::string &path) {
  if (value.is_object() && value.as_object().size() == 2U) {
    const auto *tag = value.find(tags.type);
    if (tag != nullptr && tag->is_string() && value.find(tags.value) != nullptr) {
      const auto tagged = tagged_payload(value, tags, error, path);
      if (!tagged) return std::nullopt;
      return read_shape(*tagged->payload, tagged->type, tags, error,
                        tagged->path);
    }
  }
  return read_shape(value, infer_type(value), tags, error, path);
}

std::optional<AutomationValue> read_shape(const json::Value &value,
                                          const AutomationValueType type,
                                          const AutomationValueTags tags,
                                          AutomationError &error,
                                          const std::string &path) {
  switch (type) {
  case AutomationValueType::null_value:
    if (value.is_null()) return AutomationValue{};
    break;
  case AutomationValueType::boolean:
    if (value.is_bool()) return AutomationValue(value.as_bool());
    break;
  case AutomationValueType::integer:
    if (const auto integer = read_exact_integer(value))
      return AutomationValue(*integer);
    break;
  case AutomationValueType::number:
    if (value.is_number() && std::isfinite(value.as_number()))
      return AutomationValue(value.as_number());
    break;
  case AutomationValueType::duration:
    if (const auto integer = read_exact_integer(value))
      return AutomationValue(DurationValue{Milliseconds(*integer)});
    break;
  case AutomationValueType::confidence:
    if (value.is_number() && std::isfinite(value.as_number()))
      return AutomationValue(ConfidenceValue{value.as_number()});
    break;
  case AutomationValueType::string:
    if (value.is_string()) return AutomationValue(value.as_string());
    break;
  case AutomationValueType::list:
    if (value.is_array()) {
      AutomationList result;
      result.reserve(value.as_array().size());
      for (std::size_t index{}; index < value.as_array().size(); ++index) {
        auto child = read_child(value.as_array()[index], tags, error,
                                path + "[" + std::to_string(index) + "]");
        if (!child) return std::nullopt;
        result.push_back(std::move(*child));
      }
      return AutomationValue(std::move(result));
    }
    break;
  case AutomationValueType::map:
    if (value.is_object()) {
      AutomationMap result;
      for (const auto &[key, raw] : value.as_object()) {
        auto child = read_child(raw, tags, error, path + "." + key);
        if (!child) return std::nullopt;
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

// Only run after the single bounds predicate rejects the complete value.
// Follow the original JSON shape to preserve tagged and untagged child paths;
// no bound is redefined here (including map-key and cumulative-depth bounds).
[[nodiscard]] std::string invalid_path(
    const AutomationValue &value, const json::Value &raw,
    const AutomationValueTags tags, const std::string &path,
    const std::size_t depth) {
  const auto child_path = [&](const AutomationValue &child,
                              const json::Value &child_raw,
                              const std::string &where) {
    if (child_raw.is_object() && child_raw.as_object().size() == 2U) {
      const auto *tag = child_raw.find(tags.type);
      const auto *payload = child_raw.find(tags.value);
      if (tag != nullptr && tag->is_string() && payload != nullptr)
        return invalid_path(child, *payload, tags,
                            where + "." + std::string(tags.value), depth + 1U);
    }
    return invalid_path(child, child_raw, tags, where, depth + 1U);
  };
  if (const auto *list = std::get_if<AutomationList>(&value.storage)) {
    for (std::size_t index{}; index < list->size(); ++index)
      if (!automation_value_valid_at_depth((*list)[index], depth + 1U))
        return child_path((*list)[index], raw.as_array()[index],
                          path + "[" + std::to_string(index) + "]");
  } else if (const auto *map = std::get_if<AutomationMap>(&value.storage)) {
    for (const auto &[key, child] : *map) {
      if (!automation_value_valid(AutomationValue(key)))
        return path + "." + key;
      if (!automation_value_valid_at_depth(child, depth + 1U))
        return child_path(child, *raw.find(key), path + "." + key);
    }
  }
  return path;
}

[[nodiscard]] std::optional<AutomationValue>
bounded(std::optional<AutomationValue> value, const json::Value &raw,
        const AutomationValueTags tags, AutomationError &error,
        const std::string &path) {
  if (value && !automation_value_valid(*value)) {
    error = {invalid_path(*value, raw, tags, path, 0),
             std::string(automation_value_bounds_message)};
    return std::nullopt;
  }
  return value;
}

} // namespace

json::Value automation_value_tree(const AutomationValue &value,
                                   const AutomationValueTags tags) {
  return std::visit([&](const auto &entry) -> json::Value {
    using T = std::decay_t<decltype(entry)>;
    if constexpr (std::is_same_v<T, std::int64_t>)
      return json::exact_integer_value(entry);
    else if constexpr (std::is_same_v<T, DurationValue>)
      return json::Value(entry.value.count());
    else if constexpr (std::is_same_v<T, ConfidenceValue>)
      return json::Value(entry.value);
    else if constexpr (std::is_same_v<T, AutomationList>) {
      json::Value::Array children;
      children.reserve(entry.size());
      for (const auto &child : entry)
        children.push_back(tagged_automation_value_tree(child, tags));
      return json::Value(std::move(children));
    } else if constexpr (std::is_same_v<T, AutomationMap>) {
      json::Value::Object children;
      for (const auto &[key, child] : entry)
        children.emplace(key, tagged_automation_value_tree(child, tags));
      return json::Value(std::move(children));
    } else {
      return json::Value(entry);
    }
  }, value.storage);
}

void append_automation_value(std::string &out, const AutomationValue &value,
                             const AutomationValueTags tags) {
  json::append_value(out, automation_value_tree(value, tags));
}

json::Value tagged_automation_value_tree(const AutomationValue &value,
                                          const AutomationValueTags tags) {
  return json::Value(json::Value::Object{
      {std::string(tags.type), json::Value(std::string(automation_value_type_name(value_type(value))))},
      {std::string(tags.value), automation_value_tree(value, tags)}});
}

std::optional<AutomationValue>
read_automation_value_shape(const json::Value &value,
                            const AutomationValueType type,
                            const AutomationValueTags tags,
                            AutomationError &error, const std::string &path) {
  return read_shape(value, type, tags, error, path);
}

std::optional<AutomationValue>
read_automation_value(const json::Value &value, const AutomationValueType type,
                      const AutomationValueTags tags, AutomationError &error,
                      const std::string &path) {
  return bounded(read_shape(value, type, tags, error, path), value, tags,
                 error, path);
}

std::optional<AutomationValue>
read_tagged_automation_value(const json::Value &value,
                             const AutomationValueTags tags,
                             AutomationError &error, const std::string &path) {
  const auto tagged = tagged_payload(value, tags, error, path);
  if (!tagged) return std::nullopt;
  return bounded(read_shape(*tagged->payload, tagged->type, tags, error,
                            tagged->path),
                 *tagged->payload, tags, error, tagged->path);
}

} // namespace layerc::core::detail
