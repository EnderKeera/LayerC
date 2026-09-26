#include "automation_json.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/live_edit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace layerc::core {
AutomationReplayResult replay_automation_events(
    const AutomationProfile &profile,
    const std::span<const AutomationEvent> events,
    const std::span<const AutomationReplayAssertion> assertions,
    const std::span<const CooldownGroup> cooldown_groups) {
  AutomationRuntime runtime(profile, {}, cooldown_groups);
  AutomationReplayResult result;
  for (std::size_t index{}; index < events.size(); ++index) {
    const auto evaluation = runtime.process(events[index]);
    if (!evaluation.committed)
      result.failures.push_back("event " + std::to_string(index) + " failed: " +
                                (evaluation.error.has_value()
                                     ? evaluation.error->message
                                     : "commit rejected"));
    ++result.processed_events;
  }
  result.final_state = runtime.snapshot();
  for (const auto &assertion : assertions) {
    const auto found = result.final_state.variables.find(assertion.variable_id);
    if (found == result.final_state.variables.end() ||
        found->second != assertion.expected)
      result.failures.push_back("variable assertion failed: " +
                                assertion.variable_id);
  }
  result.passed = result.failures.empty();
  return result;
}
namespace {

void append_trace_value(std::string &output, const AutomationValue &value) {
  output += "{\"type\":";
  json::append_quoted(output,
                      detail::automation_value_type_name(value_type(value)));
  output += ",\"value\":";
  if (const auto *list = std::get_if<AutomationList>(&value.storage)) {
    output.push_back('[');
    json::Join join;
    for (const auto &child : *list) {
      join(output);
      append_trace_value(output, child);
    }
    output.push_back(']');
  } else if (const auto *map = std::get_if<AutomationMap>(&value.storage)) {
    output.push_back('{');
    json::Join join;
    for (const auto &[key, child] : *map) {
      join(output);
      json::append_quoted(output, key);
      output.push_back(':');
      append_trace_value(output, child);
    }
    output.push_back('}');
  } else {
    output += automation_value_json(value);
  }
  output.push_back('}');
}

std::optional<AutomationValue> read_trace_value(const json::Value &entry,
                                                const std::string &path,
                                                const std::size_t depth,
                                                AutomationError &error) {
  if (depth > max_automation_ast_depth) {
    error = {path, "trace value nesting exceeds 16"};
    return std::nullopt;
  }
  if (!entry.is_object()) {
    error = {path, "trace value must be a typed object"};
    return std::nullopt;
  }
  const auto *type = entry.find("type");
  const auto *value = entry.find("value");
  if (type == nullptr || !type->is_string() || value == nullptr) {
    error = {path, "trace value requires type and value"};
    return std::nullopt;
  }
  const auto fail = [&](std::string message) -> std::optional<AutomationValue> {
    error = {path, std::move(message)};
    return std::nullopt;
  };
  const auto declared_type =
      detail::parse_automation_value_type_name(type->as_string());
  if (!declared_type.has_value())
    return fail("unsupported trace value type");
  switch (*declared_type) {
  case AutomationValueType::null_value:
    return value->is_null() ? std::optional(AutomationValue{})
                            : fail("expected null");
  case AutomationValueType::boolean:
    return value->is_bool() ? std::optional(AutomationValue(value->as_bool()))
                            : fail("expected Boolean");
  case AutomationValueType::integer:
  case AutomationValueType::duration: {
    const auto parsed_integer = detail::read_exact_integer(*value);
    if (!parsed_integer.has_value())
      return fail("expected exact signed integer");
    const auto integer = *parsed_integer;
    if (*declared_type == AutomationValueType::duration) {
      if (integer < 0)
        return fail("duration cannot be negative");
      return AutomationValue(DurationValue{Milliseconds(integer)});
    }
    return AutomationValue(integer);
  }
  case AutomationValueType::number:
  case AutomationValueType::confidence: {
    if (!value->is_number() || !std::isfinite(value->as_number()))
      return fail("expected finite number");
    if (*declared_type == AutomationValueType::confidence) {
      if (value->as_number() < 0.0 || value->as_number() > 1.0)
        return fail("confidence must be between 0 and 1");
      return AutomationValue(ConfidenceValue{value->as_number()});
    }
    return AutomationValue(value->as_number());
  }
  case AutomationValueType::string:
    if (!value->is_string() ||
        value->as_string().size() > max_automation_string_bytes)
      return fail("expected bounded string");
    return AutomationValue(value->as_string());
  case AutomationValueType::list: {
    if (!value->is_array() ||
        value->as_array().size() > max_automation_collection_entries)
      return fail("expected bounded list");
    AutomationList list;
    list.reserve(value->as_array().size());
    for (std::size_t index{}; index < value->as_array().size(); ++index) {
      auto child = read_trace_value(
          value->as_array()[index],
          path + ".value[" + std::to_string(index) + "]", depth + 1U, error);
      if (!child.has_value())
        return std::nullopt;
      list.push_back(std::move(*child));
    }
    return AutomationValue(std::move(list));
  }
  case AutomationValueType::map: {
    if (!value->is_object() ||
        value->as_object().size() > max_automation_collection_entries)
      return fail("expected bounded map");
    AutomationMap map;
    for (const auto &[key, child_value] : value->as_object()) {
      if (key.size() > max_automation_string_bytes)
        return fail("map key exceeds 512 UTF-8 bytes");
      auto child = read_trace_value(child_value, path + ".value." + key,
                                    depth + 1U, error);
      if (!child.has_value())
        return std::nullopt;
      map.emplace(key, std::move(*child));
    }
    return AutomationValue(std::move(map));
  }
  }
  return fail("unsupported trace value type");
}

void append_trace_entry(std::string &output,
                        const AutomationTraceEntry &entry) {
  output += "{\"sequence\":";
  detail::append_exact_unsigned(output, entry.sequence);
  output += ",\"timeMs\":";
  detail::append_exact_unsigned(
      output, static_cast<std::uint64_t>(
                  std::max<std::int64_t>(0, entry.timestamp.count())));
  output += ",\"automation\":";
  json::append_quoted(output, entry.automation_id);
  output += ",\"rule\":";
  json::append_quoted(output, entry.rule_id);
  output += ",\"phase\":";
  json::append_quoted(output, entry.phase);
  output += ",\"detail\":";
  json::append_quoted(output, entry.detail);
  if (!entry.source_event.has_value()) {
    output.push_back('}');
    return;
  }
  output += ",\"event\":";
  json::append_quoted(output,
                      detail::automation_event_name(entry.source_event->kind));
  output += ",\"source\":";
  json::append_quoted(output, entry.source_event->source_id);
  output += ",\"origin\":";
  detail::append_exact_unsigned(output, entry.source_event->origin_token);
  output += ",\"fields\":{";
  json::Join join;
  for (const auto &[name, value] : entry.source_event->fields) {
    join(output);
    json::append_quoted(output, name);
    output.push_back(':');
    append_trace_value(output, value);
  }
  output += "}}";
}
} // namespace

std::string
export_layerc_trace(const std::span<const AutomationTraceEntry> entries) {
  std::string output =
      "{\"schemaVersion\":1,\"kind\":\"layerctrace\",\"entries\":[";
  json::Join join;
  for (const auto &entry : entries) {
    join(output);
    append_trace_entry(output, entry);
  }
  output += "]}\n";
  return output;
}

std::size_t
live_trace_prefix_count(const LiveMessage &base_message,
                        const std::span<const AutomationTraceEntry> entries) {
  // JSON string escaping is per byte, so the quoted "text" field grows by the
  // escaped size of each appended entry. Serialize each entry once and pack
  // greedily instead of re-encoding whole prefixes.
  const auto escaped_size = [](const std::string_view text) {
    std::string quoted;
    json::append_quoted(quoted, text);
    return quoted.size() - 2U;
  };
  auto message = base_message;
  message.text = export_layerc_trace({});
  auto encoded_size = encode_live_message_json(message).size();
  std::string entry_json;
  std::size_t selected{};
  for (const auto &entry : entries) {
    entry_json.clear();
    if (selected != 0U)
      entry_json.push_back(',');
    append_trace_entry(entry_json, entry);
    encoded_size += escaped_size(entry_json);
    if (encoded_size > max_live_message_bytes)
      break;
    ++selected;
  }
  return selected;
}

AutomationTraceParseResult parse_layerc_trace(const std::string_view source) {
  if (source.size() > max_automation_trace_bytes)
    return AutomationError{"$", "trace exceeds the 4 MiB limit"};
  const auto parsed = json::parse_bounded(source, max_automation_trace_bytes);
  if (const auto *parse_error = std::get_if<json::FieldError>(&parsed))
    return detail::json_parse_error(*parse_error);
  const auto &root = std::get<json::Value>(parsed);
  const auto *schema = root.is_object() ? root.find("schemaVersion") : nullptr;
  const auto *kind = root.is_object() ? root.find("kind") : nullptr;
  const auto *entries = root.is_object() ? root.find("entries") : nullptr;
  if (schema == nullptr || !schema->is_number() || schema->as_number() != 1.0 ||
      kind == nullptr || !kind->is_string() ||
      kind->as_string() != "layerctrace" || entries == nullptr ||
      !entries->is_array())
    return AutomationError{"$", "invalid LayerC trace document"};
  if (entries->as_array().size() > 2048U)
    return AutomationError{"$.entries", "trace exceeds 2048 entries"};

  std::vector<AutomationEvent> events;
  for (std::size_t index{}; index < entries->as_array().size(); ++index) {
    const auto &entry = entries->as_array()[index];
    if (!entry.is_object())
      return AutomationError{"$.entries[" + std::to_string(index) + "]",
                             "trace entry must be an object"};
    const auto *phase = entry.find("phase");
    if (phase == nullptr || !phase->is_string() ||
        phase->as_string() != "source")
      continue;
    const auto path = "$.entries[" + std::to_string(index) + "]";
    const auto *event_name_value = entry.find("event");
    const auto *time = entry.find("timeMs");
    const auto *source_id = entry.find("source");
    const auto *origin = entry.find("origin");
    const auto *fields = entry.find("fields");
    const auto time_value = time == nullptr
                                ? std::optional<std::uint64_t>{}
                                : detail::read_exact_unsigned(*time);
    const auto origin_value = origin == nullptr
                                  ? std::optional<std::uint64_t>{}
                                  : detail::read_exact_unsigned(*origin);
    const auto source_entry_valid = [&] {
      return event_name_value != nullptr && event_name_value->is_string() &&
             time_value.has_value() &&
             *time_value <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()) &&
             source_id != nullptr && source_id->is_string() &&
             source_id->as_string().size() <= max_automation_string_bytes &&
             origin_value.has_value() && fields != nullptr &&
             fields->is_object() &&
             fields->as_object().size() <= max_automation_collection_entries;
    };
    if (!source_entry_valid())
      return AutomationError{path, "invalid source trace entry"};
    const auto event_kind =
        detail::parse_automation_event_name(event_name_value->as_string());
    if (!event_kind.has_value())
      return AutomationError{path + ".event", "unsupported event kind"};
    AutomationEvent event;
    event.kind = *event_kind;
    event.timestamp =
        TimePoint(Milliseconds(static_cast<std::int64_t>(*time_value)));
    event.source_id = source_id->as_string();
    event.origin_token = *origin_value;
    for (const auto &[name, value] : fields->as_object()) {
      if (name.size() > max_automation_string_bytes)
        return AutomationError{path + ".fields",
                               "field name exceeds 512 UTF-8 bytes"};
      AutomationError value_error;
      auto parsed_value =
          read_trace_value(value, path + ".fields." + name, 0, value_error);
      if (!parsed_value.has_value())
        return value_error;
      event.fields.emplace(name, std::move(*parsed_value));
    }
    events.push_back(std::move(event));
  }
  return events;
}
} // namespace layerc::core
