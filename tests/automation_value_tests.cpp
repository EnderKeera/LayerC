#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

GameProfile value_base() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  return profile;
}

AutomationValue deeply_nested_value(const std::size_t depth) {
  AutomationValue value(false);
  for (std::size_t index{}; index < depth; ++index) {
    AutomationList list;
    list.push_back(std::move(value));
    value = AutomationValue(std::move(list));
  }
  return value;
}

} // namespace

void test_automation_value_round_trip_and_bounds() {
  const auto base = value_base();
  AutomationMap record;
  record.emplace("minimum",
                 AutomationValue(std::numeric_limits<std::int64_t>::min()));
  record.emplace("label", AutomationValue(std::string("caf\xC3\xA9")));
  AutomationList mixed;
  mixed.emplace_back(std::numeric_limits<std::int64_t>::max());
  mixed.emplace_back(DurationValue{123ms});
  mixed.emplace_back(ConfidenceValue{0.875});
  mixed.emplace_back(std::move(record));

  AutomationProfile profile;
  profile.profile_id = base.id;
  profile.variables = {{.id = "mixed",
                        .type = AutomationValueType::list,
                        .default_value = AutomationValue(mixed),
                        .persistent = true},
                       {.id = "maximum",
                        .type = AutomationValueType::integer,
                        .default_value = AutomationValue(
                            std::numeric_limits<std::int64_t>::max()),
                        .persistent = true}};

  const auto serialized = serialize_automation_profile_json(profile);
  const auto parsed = parse_automation_profile_json(serialized, base);
  const auto *round_trip = std::get_if<AutomationProfile>(&parsed);
  expect(round_trip != nullptr && round_trip->variables == profile.variables,
         "heterogeneous nested values and full int64 bounds should round trip");

  for (const std::string malformed :
       {std::string{}, std::string{"{"}, std::string{"[]"},
        std::string{R"({"schemaVersion":2})"},
        std::string{"{\"schemaVersion\":1,\"profileId\":\"\xC3\x28\"}"}})
    expect(std::holds_alternative<AutomationError>(
               parse_automation_profile_json(malformed, base)),
           "malformed automation documents should fail closed");

  // A deterministic mutation sweep is a cheap parser-fuzz regression: every
  // corrupted byte must either be rejected or still produce a fully valid
  // bounded graph, never an unchecked partial profile.
  for (std::size_t index{}; index < serialized.size(); ++index) {
    auto mutated = serialized;
    mutated[index] =
        static_cast<char>(static_cast<unsigned char>(mutated[index]) ^ 0x80U);
    const auto result = parse_automation_profile_json(mutated, base);
    if (const auto *candidate = std::get_if<AutomationProfile>(&result))
      expect(validate_automation_profile(*candidate, base).message.empty(),
             "accepted automation mutations must remain statically valid");
  }
  for (std::size_t size{}; size < serialized.size(); size += 7U)
    expect(
        std::holds_alternative<AutomationError>(parse_automation_profile_json(
            std::string_view(serialized).substr(0, size), base)),
        "truncated automation documents should fail closed");
  const AutomationPersistentValues state{
      {"mixed", AutomationValue(mixed)},
      {"maximum", AutomationValue(std::numeric_limits<std::int64_t>::max())}};
  const auto state_json = serialize_automation_state_json(profile, state);
  const auto parsed_state = parse_automation_state_json(state_json, profile);
  const auto *state_values =
      std::get_if<AutomationPersistentValues>(&parsed_state);
  expect(state_values != nullptr && *state_values == state,
         "persistent automation state should preserve nested types and int64");

  expect(std::get<AutomationValue>(parse_automation_value_json(
             "\"9223372036854775807\"", AutomationValueType::integer)) ==
             AutomationValue(std::numeric_limits<std::int64_t>::max()),
         "quoted exact integers should cover the full signed 64-bit range");
  expect(std::holds_alternative<AutomationError>(parse_automation_value_json(
             "9223372036854775808", AutomationValueType::integer)),
         "imprecise or out-of-range JSON numbers should be rejected");

  AutomationEvent event;
  event.kind = AutomationEventKind::visual_observation;
  event.timestamp = TimePoint(321ms);
  event.source_id = "locator";
  event.origin_token = std::numeric_limits<std::uint64_t>::max();
  event.fields.emplace("mixed", AutomationValue(mixed));
  AutomationTraceEntry entry;
  entry.sequence = std::numeric_limits<std::uint64_t>::max();
  entry.timestamp = 321ms;
  entry.phase = "source";
  entry.detail = "typed source";
  entry.source_event = event;
  const auto trace = export_layerc_trace({&entry, 1U});
  const auto parsed_trace = parse_layerc_trace(trace);
  const auto *events = std::get_if<std::vector<AutomationEvent>>(&parsed_trace);
  expect(events != nullptr && events->size() == 1U &&
             events->front().origin_token == event.origin_token &&
             events->front().fields == event.fields,
         "trace replay should preserve exact origins and nested typed fields");

  auto invalid = profile;
  invalid.variables.front().default_value =
      deeply_nested_value(max_automation_ast_depth + 2U);
  expect(!validate_automation_profile(invalid, base).message.empty(),
         "programmatic values deeper than the schema limit should fail");

  invalid = profile;
  invalid.variables.front().default_value = AutomationValue(AutomationList(
      max_automation_collection_entries + 1U, AutomationValue(false)));
  expect(!validate_automation_profile(invalid, base).message.empty(),
         "programmatic collections beyond 64 entries should fail");

  invalid = profile;
  invalid.variables.push_back(
      {.id = "bad-text",
       .type = AutomationValueType::string,
       .default_value = AutomationValue(std::string("\xC3\x28"))});
  expect(!validate_automation_profile(invalid, base).message.empty(),
         "programmatic invalid UTF-8 should fail static validation");

  AutomationRuntime runtime(profile);
  AutomationEvent oversized;
  oversized.kind = AutomationEventKind::session_started;
  for (std::size_t index{}; index <= max_automation_collection_entries; ++index)
    oversized.fields.emplace("field-" + std::to_string(index),
                             AutomationValue(false));
  const auto rejected = runtime.process(oversized);
  expect(!rejected.committed && rejected.error.has_value(),
         "runtime source events must enforce collection bounds");
}

} // namespace layerc::test