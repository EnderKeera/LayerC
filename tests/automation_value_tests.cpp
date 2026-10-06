#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

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

  for (const std::string &malformed :
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
  entry.phase = "root";
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

  AutomationRuntime runtime(profile, {}, {}, [] { return TimePoint{}; });
  AutomationEvent oversized;
  oversized.kind = AutomationEventKind::session_started;
  for (std::size_t index{}; index <= max_automation_collection_entries; ++index)
    oversized.fields.emplace("field-" + std::to_string(index),
                             AutomationValue(false));
  const auto rejected = runtime.process(oversized);
  expect(!rejected.committed && rejected.error.has_value(),
         "runtime source events must enforce collection bounds");
}

void test_automation_mutation_postconditions() {
  auto base = value_base();
  base.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = 5s});
  for (const auto kind : {AutomationActionKind::append_list,
                          AutomationActionKind::set_map_entry}) {
    AutomationProfile profile;
    profile.profile_id = base.id;
    const bool list = kind == AutomationActionKind::append_list;
    profile.variables = {
        {.id = "target",
         .type = list ? AutomationValueType::list : AutomationValueType::map,
         .default_value = list ? AutomationValue(AutomationList{})
                               : AutomationValue(AutomationMap{}),
         .persistent = true},
        {.id = "deep",
         .type = AutomationValueType::list,
         .default_value = deeply_nested_value(max_automation_ast_depth)},
        {.id = "healthy",
         .type = AutomationValueType::integer,
         .default_value = AutomationValue(std::int64_t{}),
         .persistent = true}};
    profile.automations = {
        {.id = "invalid",
         .name = "Invalid mutation",
         .rules = {{.id = "mutate",
                    .event = AutomationEventKind::session_started,
                    .actions = {{.kind = kind,
                                 .target = "target",
                                 .value = expr::variable("deep"),
                                 .secondary =
                                     list ? std::nullopt
                                          : std::optional(
                                                expr::literal(AutomationValue(
                                                    std::string("child"))))},
                                {.kind = AutomationActionKind::reset_cooldown,
                                 .target = "dash"}}}}},
        {.id = "healthy",
         .name = "Healthy",
         .rules = {{.id = "set",
                    .event = AutomationEventKind::session_started,
                    .actions = {{.kind = AutomationActionKind::set_variable,
                                 .target = "healthy",
                                 .value = expr::literal(
                                     AutomationValue(std::int64_t{1}))}}}}}};
    expect(validate_automation_profile(profile, base).message.empty(),
           "AR-02 mutation fixture must be statically valid");
    unsigned external_effects{};
    AutomationRuntime runtime(profile, {}, {}, [] { return TimePoint{}; });
    const auto result = runtime.process(
        {.kind = AutomationEventKind::session_started}, {},
        [&](std::span<const AutomationEffect> effects) {
          external_effects += static_cast<unsigned>(
              std::ranges::count(effects, AutomationActionKind::reset_cooldown,
                                 &AutomationEffect::kind));
          return true;
        });
    expect(result.committed &&
               result.faulted_automations ==
                   std::vector<std::string>{"invalid"} &&
               external_effects == 0U &&
               runtime.snapshot().variables.at("target") ==
                   profile.variables.front().default_value &&
               runtime.snapshot().variables.at("healthy") ==
                   AutomationValue(std::int64_t{1}),
           "AR-02 depth-17 mutation and its external effects must roll back "
           "only their owner");
    expect(result.derived_events.size() == 1U &&
               result.derived_events.front().source_id == "healthy",
           "AR-02 rejected mutations must not publish derived variable events");
    const auto state = runtime.persistent_values();
    const auto decoded = parse_automation_state_json(
        serialize_automation_state_json(profile, state), profile);
    expect(
        state.contains("target") &&
            std::holds_alternative<AutomationPersistentValues>(decoded) &&
            std::get<AutomationPersistentValues>(decoded) == state,
        "AR-02 no committed persistent value may be omitted by serialization");
  }

  AutomationProfile recursive;
  recursive.profile_id = base.id;
  recursive.variables = {{.id = "record",
                          .type = AutomationValueType::map,
                          .default_value = AutomationValue(AutomationMap{}),
                          .persistent = true}};
  recursive.automations = {
      {.id = "self",
       .name = "Self nesting",
       .rules = {{.id = "nest",
                  .event = AutomationEventKind::session_started,
                  .actions = {{.kind = AutomationActionKind::set_map_entry,
                               .target = "record",
                               .value = expr::variable("record"),
                               .secondary = expr::literal(
                                   AutomationValue(std::string("self")))},
                              {.kind = AutomationActionKind::reset_cooldown,
                               .target = "dash"}}}}}};
  AutomationRuntime nesting(recursive, {}, {}, [] { return TimePoint{}; });
  unsigned commits{};
  const auto commit = [&](std::span<const AutomationEffect> effects) {
    commits += static_cast<unsigned>(
        std::ranges::count(effects, AutomationActionKind::reset_cooldown,
                           &AutomationEffect::kind));
    return true;
  };
  for (std::size_t depth{}; depth < max_automation_ast_depth; ++depth)
    expect(
        nesting
            .process({.kind = AutomationEventKind::session_started}, {}, commit)
            .committed,
        "AR-02 valid recursive map depths must still commit");
  const auto before = nesting.persistent_values();
  const auto rejected = nesting.process(
      {.kind = AutomationEventKind::session_started}, {}, commit);
  expect(rejected.committed &&
             rejected.faulted_automations == std::vector<std::string>{"self"} &&
             commits == max_automation_ast_depth &&
             nesting.persistent_values() == before &&
             rejected.derived_events.empty(),
         "AR-02 self-nesting must stop at the canonical bound without "
         "persistence loss");
}

void test_automation_clock_boundaries() {
  const auto maximum_integer = std::numeric_limits<std::int64_t>::max();
  const bool huge_rejected =
      !as_duration(AutomationValue(maximum_integer)).has_value();
  expect(
      huge_rejected,
      "AR-03 INT64_MAX milliseconds must be rejected before native conversion");
  // Keep this regression safe even against the old implementation: never
  // execute the overflowing chrono conversion it previously admitted.
  if (!huge_rejected)
    return;
  const auto largest =
      std::chrono::duration_cast<Milliseconds>(TimePoint::duration::max());
  expect(largest.count() < maximum_integer,
         "native clocks must have a narrower millisecond range");
  if (largest.count() == maximum_integer)
    return;
  const auto too_large = Milliseconds(largest.count() + 1);
  expect(automation_time_after(TimePoint{}, largest).has_value() &&
             !automation_time_after(TimePoint{}, too_large) &&
             !as_duration(AutomationValue(too_large.count())) &&
             !automation_time_after(TimePoint{}, -1ms),
         "AR-03 exact representable duration boundary must be checked");
  const auto near_end =
      TimePoint::max() - std::chrono::duration_cast<TimePoint::duration>(49ms);
  expect(automation_time_after(near_end, 49ms) == TimePoint::max() &&
             !automation_time_after(near_end, 50ms) &&
             automation_time_after(TimePoint::min(), 50ms).has_value(),
         "AR-03 addition must reject overflow without rejecting valid negative "
         "epochs");

  auto base = value_base();
  base.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = 5s});
  base.panels.push_back({.id = "main", .name = "Main"});
  for (const auto kind : {AutomationActionKind::create_timer,
                          AutomationActionKind::set_overlay_override}) {
    for (const auto duration :
         {Milliseconds(maximum_integer), too_large, 50ms}) {
      const auto start = duration == 50ms ? near_end : TimePoint{};
      AutomationProfile profile;
      profile.profile_id = base.id;
      const bool timer = kind == AutomationActionKind::create_timer;
      AutomationAction action{
          .kind = kind,
          .target = timer ? "tick" : "main",
          .name = timer ? "" : "visibility",
          .property = timer ? "" : "visible",
          .value = expr::literal(timer ? AutomationValue(duration.count())
                                       : AutomationValue(false)),
          .secondary = timer ? std::nullopt
                             : std::optional(expr::literal(
                                   AutomationValue(duration.count())))};
      profile.automations = {
          {.id = "clock",
           .name = "Clock",
           .rules = {{.id = "start",
                      .event = AutomationEventKind::session_started,
                      .actions = {action,
                                  {.kind = AutomationActionKind::reset_cooldown,
                                   .target = "dash"}}}}}};
      expect(
          validate_automation_profile(profile, base).message.empty(),
          "AR-03 integer duration fixture must remain statically compatible");
      AutomationRuntime runtime(profile, {}, {}, [] { return TimePoint{}; });
      unsigned commits{};
      const auto result = runtime.process(
          {.kind = AutomationEventKind::session_started, .timestamp = start},
          {}, [&](std::span<const AutomationEffect> effects) {
            if (!effects.empty())
              ++commits;
            return true;
          });
      expect(result.committed &&
                 result.faulted_automations ==
                     std::vector<std::string>{"clock"} &&
                 commits == 0U && runtime.snapshot().timers.empty() &&
                 runtime.snapshot().overlay_overrides.empty(),
             "AR-03 timers and override expiries must reject "
             "conversion/addition overflow before commit");
    }
  }

  AutomationProfile repeating;
  repeating.profile_id = base.id;
  repeating.automations = {
      {.id = "repeat",
       .name = "Repeat",
       .rules = {{.id = "start",
                  .event = AutomationEventKind::session_started,
                  .actions = {{.kind = AutomationActionKind::create_timer,
                               .target = "tick",
                               .value = expr::literal(
                                   AutomationValue(std::int64_t{50})),
                               .repeating = true}}},
                 {.id = "fire",
                  .event = AutomationEventKind::timer_fired,
                  .source_filter = "tick",
                  .actions = {{.kind = AutomationActionKind::reset_cooldown,
                               .target = "dash"}}}}}};
  AutomationRuntime rearm(repeating, {}, {}, [] { return TimePoint{}; });
  expect(
      rearm.process({.kind = AutomationEventKind::session_started}).committed,
      "AR-03 representable repeating timers must start");
  const auto due = rearm.next_timer_due();
  unsigned rearm_commits{};
  const auto failed_rearm =
      rearm.process({.kind = AutomationEventKind::timer_fired,
                     .timestamp = near_end,
                     .source_id = "tick"},
                    {}, [&](std::span<const AutomationEffect>) {
                      ++rearm_commits;
                      return true;
                    });
  expect(!failed_rearm.committed && failed_rearm.error && rearm_commits == 0U &&
             rearm.next_timer_due() == due &&
             failed_rearm.faulted_automations.empty(),
         "AR-03 overflowing rearm must leave the timer and external effects "
         "untouched");

  AutomationTraceEntry entry;
  entry.phase = "root";
  entry.source_event =
      AutomationEvent{.kind = AutomationEventKind::session_started};
  entry.timestamp = largest;
  expect(std::holds_alternative<std::vector<AutomationEvent>>(
             parse_layerc_trace(export_layerc_trace({&entry, 1U}))),
         "AR-03 the largest representable trace timestamp must import");
  for (const auto timestamp : {too_large, Milliseconds(maximum_integer)}) {
    entry.timestamp = timestamp; // Milliseconds storage itself does not convert
                                 // to native ticks.
    const auto trace = parse_layerc_trace(export_layerc_trace({&entry, 1U}));
    expect(std::holds_alternative<AutomationError>(trace) &&
               std::get<AutomationError>(trace).path == "$.entries[0].timeMs",
           "AR-03 oversized trace timestamps must be rejected before native "
           "construction");
  }
}

void test_automation_decode_postconditions() {
  struct Case {
    AutomationValueType type;
    AutomationValue valid;
    std::string invalid;
    std::string suffix;
  };
  const std::vector<Case> cases{
      {AutomationValueType::confidence, AutomationValue(ConfidenceValue{0.5}),
       "2", ""},
      {AutomationValueType::confidence, AutomationValue(ConfidenceValue{1.0}),
       "-0.1", ""},
      {AutomationValueType::duration,
       AutomationValue(DurationValue{86400000ms}), "86400001", ""},
      {AutomationValueType::duration, AutomationValue(DurationValue{0ms}), "-1",
       ""},
      {AutomationValueType::string, AutomationValue(std::string(512U, 'a')),
       automation_value_json(AutomationValue(std::string(513U, 'a'))), ""},
      {AutomationValueType::list, deeply_nested_value(16U),
       automation_value_json(deeply_nested_value(17U)), "[0]"},
      {AutomationValueType::list,
       AutomationValue(AutomationList(64U, AutomationValue(false))),
       automation_value_json(
           AutomationValue(AutomationList(65U, AutomationValue(false)))),
       ""},
      {AutomationValueType::list, AutomationValue(AutomationList{}),
       R"([{"$type":"confidence","$value":2}])", "[0].$value"},
      {AutomationValueType::map, AutomationValue(AutomationMap{}),
       R"({"child":{"$type":"duration","$value":86400001}})", ".child.$value"},
      {AutomationValueType::map,
       AutomationValue(
           AutomationMap{{std::string(512U, 'k'), AutomationValue(false)}}),
       automation_value_json(AutomationValue(
           AutomationMap{{std::string(513U, 'k'), AutomationValue(false)}})),
       "." + std::string(513U, 'k')}};
  const auto base = value_base();
  for (const auto &test : cases) {
    const auto decoded = parse_automation_value_json(test.invalid, test.type);
    expect(
        std::holds_alternative<AutomationError>(decoded),
        "AR-05 invalid bounded values must never be reported as parse success");
    if (const auto *error = std::get_if<AutomationError>(&decoded))
      expect(error->path.starts_with("$" + test.suffix) &&
                 !error->message.empty(),
             "AR-05 decoding must identify the invalid value's path");
    const auto valid = parse_automation_value_json(
        automation_value_json(test.valid), test.type);
    expect(
        std::holds_alternative<AutomationValue>(valid) &&
            automation_value_valid(std::get<AutomationValue>(valid)) &&
            std::get<AutomationValue>(valid) == test.valid,
        "AR-05 accepted boundary values must remain canonical and byte-stable");
    // Trace tags differ, but the bound owner must be the same as for state.
    auto trace_payload = test.invalid;
    for (const auto &tag : {std::string("$type"), std::string("$value")}) {
      std::size_t offset{};
      while ((offset = trace_payload.find("\"" + tag + "\"", offset)) !=
             std::string::npos)
        trace_payload.erase(offset + 1U, 1U);
    }
    const auto bad_trace =
        std::string(R"({"schemaVersion":1,"kind":"layerctrace","entries":[{"phase":"root","event":"inputPressed","timeMs":0,"source":"x","origin":1,"fields":{"bad":{"type":")") +
        std::string(automation_value_type_name(test.type)) +
        R"(","value":)" + trace_payload + "}}}]}";
    const auto rejected_trace = parse_layerc_trace(bad_trace);
    expect(std::holds_alternative<AutomationError>(rejected_trace),
           "trace and state codecs must reject the same invalid bounded values");
    if (const auto *error = std::get_if<AutomationError>(&rejected_trace))
      expect(error->path.starts_with("$.entries[0].fields.bad.value"),
             "trace codec must locate the invalid payload, not its envelope");
    AutomationEvent trace_event;
    trace_event.fields.emplace("valid", test.valid);
    const auto trace_entry = make_automation_root_trace_entry(trace_event, 1U);
    const auto valid_trace = parse_layerc_trace(
        export_layerc_trace({&trace_entry, 1U}));
    const auto *events = std::get_if<std::vector<AutomationEvent>>(&valid_trace);
    expect(events != nullptr && events->size() == 1U &&
               events->front().fields.at("valid") == test.valid,
           "trace codec must preserve the same accepted boundary values");

    AutomationProfile profile;
    profile.profile_id = base.id;
    profile.variables = {{.id = "bad",
                          .type = test.type,
                          .default_value = test.valid,
                          .persistent = true}};
    const auto state = parse_automation_state_json(
        "{\"schemaVersion\":1,\"profileId\":\"game\",\"values\":{\"bad\":" +
            test.invalid + "}}",
        profile);
    expect(std::holds_alternative<AutomationError>(state) &&
               std::get<AutomationError>(state).path.starts_with(
                   "$.values.bad" + test.suffix),
           "AR-05 invalid persistent values must reject the state sidecar "
           "instead of resetting defaults");
    auto document = serialize_automation_profile_json(profile);
    const auto original = automation_value_json(test.valid);
    const auto offset = document.find("\"default\":" + original);
    expect(offset != std::string::npos,
           "AR-05 fixture must contain its typed default");
    if (offset != std::string::npos) {
      document.replace(offset + 10U, original.size(), test.invalid);
      const auto parsed = parse_automation_profile_json(document, base);
      expect(std::holds_alternative<AutomationError>(parsed) &&
                 std::get<AutomationError>(parsed).path.starts_with(
                     "$.variables[0].default"),
             "AR-05 invalid defaults retain the existing automation-sidecar "
             "rejection scope");
    }
  }
}

} // namespace layerc::test
