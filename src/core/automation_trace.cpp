#include "automation_json.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace layerc::core {
AutomationReplayResult replay_automation_events(
    const AutomationProfile &profile,
    const std::span<const AutomationEvent> events,
    const std::span<const AutomationReplayAssertion> assertions,
    const std::span<const CooldownGroup> cooldown_groups) {
  // Replay is an offline reproduction. Deterministic node-evaluation and
  // derived-event caps bound its work, so host scheduling latency must not
  // decide its verdict through the live loop's wall-clock execution budget.
  AutomationRuntime runtime(profile, {}, cooldown_groups,
                            [] { return TimePoint{}; });
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
json::Value trace_entry_tree(const AutomationTraceEntry &entry) {
  using json::Value;
  Value::Object object{
      {"sequence", json::exact_unsigned_value(entry.sequence)},
      {"timeMs", json::exact_unsigned_value(static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, entry.timestamp.count())))},
      {"automation", Value(entry.automation_id)}, {"rule", Value(entry.rule_id)},
      {"phase", Value(entry.phase)}, {"detail", Value(entry.detail)}};
  if (entry.source_event) {
    object.emplace("event", Value(std::string(automation_event_name(entry.source_event->kind))));
    object.emplace("source", Value(entry.source_event->source_id));
    object.emplace("origin", json::exact_unsigned_value(entry.source_event->origin_token));
    Value::Object fields;
    for (const auto &[name, value] : entry.source_event->fields)
      fields.emplace(name, detail::tagged_automation_value_tree(value, detail::trace_value_tags));
    object.emplace("fields", Value(std::move(fields)));
  }
  return Value(std::move(object));
}
} // namespace

void append_layerc_trace_entry(std::string &output,
                               const AutomationTraceEntry &entry) {
  json::append_value(output, trace_entry_tree(entry));
}

AutomationTraceEntry
make_automation_root_trace_entry(const AutomationEvent &event,
                                const std::uint64_t sequence) {
  AutomationTraceEntry entry;
  entry.sequence = sequence;
  entry.timestamp = std::chrono::duration_cast<Milliseconds>(
      event.timestamp.time_since_epoch());
  entry.phase = automation_trace_root_phase;
  entry.detail = "recorded event";
  entry.source_event = event;
  return entry;
}

std::string export_layerc_trace(const std::span<const AutomationTraceEntry> entries) {
  using json::Value;
  Value::Array array;
  array.reserve(entries.size());
  for (const auto &entry : entries)
    array.push_back(trace_entry_tree(entry));
  std::string output;
  json::append_value(output, Value(Value::Object{
      {"schemaVersion", Value(std::int64_t{1})}, {"kind", Value(std::string("layerctrace"))},
      {"entries", Value(std::move(array))}}));
  output.push_back('\n');
  return output;
}

AutomationTraceParseResult parse_layerc_trace(const std::string_view source) {
  const auto parsed = json::parse_bounded(source, max_automation_trace_bytes);
  if (const auto *parse_error = std::get_if<json::FieldError>(&parsed))
    return *parse_error;
  const auto &root = std::get<json::Value>(parsed);
  const auto *schema = root.is_object() ? root.find("schemaVersion") : nullptr;
  const auto *kind = root.is_object() ? root.find("kind") : nullptr;
  const auto *entries = root.is_object() ? root.find("entries") : nullptr;
  if (schema == nullptr || !schema->is_number() || schema->as_number() != 1.0 ||
      kind == nullptr || !kind->is_string() ||
      kind->as_string() != "layerctrace" || entries == nullptr ||
      !entries->is_array())
    return AutomationError{"$", "invalid LayerC trace document"};
  if (entries->as_array().size() > max_automation_trace_entries)
    return AutomationError{"$.entries", "trace exceeds 2048 entries"};

  std::vector<AutomationEvent> events;
  for (std::size_t index{}; index < entries->as_array().size(); ++index) {
    const auto &entry = entries->as_array()[index];
    if (!entry.is_object())
      return AutomationError{"$.entries[" + std::to_string(index) + "]",
                             "trace entry must be an object"};
    const auto path = "$.entries[" + std::to_string(index) + "]";
    const auto *phase = entry.find("phase");
    if (phase != nullptr && phase->is_string() &&
        phase->as_string() == "source")
      return AutomationError{
          path + ".phase",
          "legacy source traces lack root provenance; capture a new trace"};
    if (phase == nullptr || !phase->is_string() ||
        phase->as_string() != automation_trace_root_phase)
      continue; // derived entries and condition/error audit are never replay
                // roots
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
        parse_automation_event_name(event_name_value->as_string());
    if (!event_kind.has_value())
      return AutomationError{path + ".event", "unsupported event kind"};
    const auto timestamp = automation_time_after(
        TimePoint{}, Milliseconds(static_cast<std::int64_t>(*time_value)));
    if (!timestamp)
      return AutomationError{path + ".timeMs",
                             "trace timestamp is outside the clock range"};
    AutomationEvent event;
    event.kind = *event_kind;
    event.timestamp = *timestamp;
    event.source_id = source_id->as_string();
    event.origin_token = *origin_value;
    for (const auto &[name, value] : fields->as_object()) {
      if (name.size() > max_automation_string_bytes)
        return AutomationError{path + ".fields",
                               "field name exceeds 512 UTF-8 bytes"};
      AutomationError value_error;
      auto parsed_value = detail::read_tagged_automation_value(
          value, detail::trace_value_tags, value_error,
          path + ".fields." + name);
      if (!parsed_value.has_value())
        return value_error;
      event.fields.emplace(name, std::move(*parsed_value));
    }
    events.push_back(std::move(event));
  }
  return events;
}
} // namespace layerc::core
