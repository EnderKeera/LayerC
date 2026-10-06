#include "automation_json.hpp"

#include "layerc/core/json.hpp"

#include <cmath>
#include <type_traits>
#include <utility>

// The one AutomationValue JSON codec. Profile literals, persisted state, and
// traces differ only in their tag keys; every bound comes from
// automation_value_valid, and the JSON parser bounds nesting.
namespace layerc::core::detail {
namespace {

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
    if (tag != nullptr && tag->is_string() && value.find(tags.value) != nullptr)
      return read_tagged_automation_value(value, tags, error, path);
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
             "value exceeds automation bounds: depth 16, 64 entries, "
                   "512-byte UTF-8 strings and keys, duration 0 through "
                   "86400000 ms, confidence 0 through 1"};
    return std::nullopt;
  }
  return value;
}

} // namespace

void append_automation_value(std::string &out, const AutomationValue &value,
                             const AutomationValueTags tags) {
  std::visit(
      [&](const auto &entry) {
        using T = std::decay_t<decltype(entry)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>)
          out += "null";
        else if constexpr (std::is_same_v<T, bool>)
          json::append_bool(out, entry);
        else if constexpr (std::is_same_v<T, std::int64_t>)
          append_exact_integer(out, entry);
        else if constexpr (std::is_same_v<T, double>)
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
            append_tagged_automation_value(out, child, tags);
          }
          out.push_back(']');
        } else if constexpr (std::is_same_v<T, AutomationMap>) {
          out.push_back('{');
          json::Join join;
          for (const auto &[key, child] : entry) {
            join(out);
            json::append_quoted(out, key);
            out.push_back(':');
            append_tagged_automation_value(out, child, tags);
          }
          out.push_back('}');
        }
      },
      value.storage);
}

void append_tagged_automation_value(std::string &out,
                                    const AutomationValue &value,
                                    const AutomationValueTags tags) {
  out.push_back('{');
  json::append_quoted(out, tags.type);
  out.push_back(':');
  json::append_quoted(out, automation_value_type_name(value_type(value)));
  out.push_back(',');
  json::append_quoted(out, tags.value);
  out.push_back(':');
  append_automation_value(out, value, tags);
  out.push_back('}');
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
  const auto payload_path = path + "." + std::string(tags.value);
  return bounded(read_shape(*payload, *type, tags, error, payload_path),
                 *payload, tags, error, payload_path);
}

} // namespace layerc::core::detail
