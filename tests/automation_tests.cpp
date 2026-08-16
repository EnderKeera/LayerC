#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/live_edit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

AutomationExpression literal(AutomationValue value) {
  return {.kind = ExpressionKind::literal, .literal = std::move(value)};
}

AutomationExpression variable(std::string id) {
  return {.kind = ExpressionKind::variable, .reference = std::move(id)};
}

GameProfile automation_base() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = 5s});
  profile.cooldown_groups.push_back(
      {.id = "movement",
       .name = "Movement",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"dash"}});
  return profile;
}

AutomationProfile automation_fixture() {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0}),
       .persistent = true});
  profile.variables.push_back(
      {.id = "items",
       .type = AutomationValueType::list,
       .default_value = AutomationValue(AutomationList{})});

  AutomationRule rule;
  rule.id = "increment";
  rule.priority = 10;
  rule.event = AutomationEventKind::input_pressed;
  rule.source_filter = "key-e";
  rule.condition = {.kind = ExpressionKind::binary,
                    .operation = "less",
                    .arguments = {variable("count"),
                                  literal(AutomationValue(std::int64_t{3}))}};
  rule.actions.push_back({.kind = AutomationActionKind::increment_variable,
                          .target = "count",
                          .value = literal(AutomationValue(std::int64_t{1}))});
  rule.actions.push_back(
      {.kind = AutomationActionKind::trigger_cooldown, .target = "dash"});
  rule.actions.push_back(
      {.kind = AutomationActionKind::create_timer,
       .target = "pulse",
       .value = literal(AutomationValue(DurationValue{50ms}))});
  profile.automations.push_back(
      {.id = "basic", .name = "Basic", .enabled = true, .rules = {rule}});
  return profile;
}
} // namespace

void test_automation_profile_round_trip() {
  using namespace layerc::core;
  const auto base = automation_base();
  const auto source = automation_fixture();
  const auto validation = validate_automation_profile(source, base);
  expect(validation.message.empty(),
         "a bounded typed automation fixture should validate");

  const auto json = serialize_automation_profile_json(source);
  const auto parsed = parse_automation_profile_json(json, base);
  const auto *profile = std::get_if<AutomationProfile>(&parsed);
  expect(profile != nullptr, "serialized automation schema 1 should parse");
  if (profile != nullptr) {
    expect(profile->continuous_input == source.continuous_input,
           "automation continuous-input settings should round trip");
    expect(profile->variables == source.variables,
           "automation variables should round trip");
    expect(profile->automations.size() == source.automations.size(),
           "automation collection size should round trip");
    if (profile->automations.size() == source.automations.size() &&
        !profile->automations.empty()) {
      const auto &actual = profile->automations.front();
      const auto &expected = source.automations.front();
      expect(actual.id == expected.id && actual.name == expected.name &&
                 actual.enabled == expected.enabled,
             "automation identity should round trip");
      expect(actual.rules.size() == expected.rules.size(),
             "automation rule count should round trip");
      if (actual.rules.size() == expected.rules.size() &&
          !actual.rules.empty()) {
        const auto &actual_rule = actual.rules.front();
        const auto &expected_rule = expected.rules.front();
        expect(actual_rule.id == expected_rule.id &&
                   actual_rule.priority == expected_rule.priority &&
                   actual_rule.event == expected_rule.event &&
                   actual_rule.source_filter == expected_rule.source_filter,
               "automation rule metadata should round trip");
        expect(actual_rule.condition.kind == expected_rule.condition.kind,
               "automation condition kind should round trip");
        expect(actual_rule.condition.operation ==
                   expected_rule.condition.operation,
               "automation condition operation should round trip");
        expect(actual_rule.condition.reference ==
                   expected_rule.condition.reference,
               "automation condition reference should round trip");
        expect(actual_rule.condition.literal == expected_rule.condition.literal,
               "automation condition unused literal should round trip");
        expect(actual_rule.condition.arguments.size() ==
                   expected_rule.condition.arguments.size(),
               "automation condition arguments should round trip");
        if (actual_rule.condition.arguments.size() ==
            expected_rule.condition.arguments.size()) {
          for (std::size_t argument{};
               argument < actual_rule.condition.arguments.size(); ++argument)
            expect(actual_rule.condition.arguments[argument] ==
                       expected_rule.condition.arguments[argument],
                   "each automation condition argument should round trip");
        }
        expect(actual_rule.actions == expected_rule.actions,
               "automation rule actions should round trip");
      }
    }
  }

  auto wrong_base = base;
  wrong_base.id = "other";
  expect(std::holds_alternative<AutomationError>(
             parse_automation_profile_json(json, wrong_base)),
         "automation sidecars must match their base profile ID");

  auto invalid = source;
  invalid.automations.front().rules.front().actions.front().target = "missing";
  expect(!validate_automation_profile(invalid, base).message.empty(),
         "static validation should reject unknown action targets");
}

void test_automation_runtime_transaction_and_replay() {
  using namespace layerc::core;
  const auto profile = automation_fixture();
  AutomationRuntime runtime(profile);
  runtime.set_trace_enabled(true);

  std::vector<AutomationEffect> committed;
  AutomationEvent event{AutomationEventKind::input_pressed, TimePoint{},
                        "key-e", 42};
  event.fields.emplace("confidence", AutomationValue(ConfidenceValue{0.75}));
  event.fields.emplace("delay", AutomationValue(DurationValue{25ms}));
  const auto result = runtime.process(
      event, {}, [&](const std::span<const AutomationEffect> effects) {
        committed.insert(committed.end(), effects.begin(), effects.end());
        return true;
      });
  const auto count = runtime.snapshot().variables.find("count");
  expect(result.committed && result.effects.size() == 3 &&
             committed.size() == 3 &&
             count != runtime.snapshot().variables.end() &&
             count->second == AutomationValue(std::int64_t{1}),
         "one root event should validate and commit its complete transaction");
  expect(runtime.persistent_dirty() && runtime.next_timer_due().has_value() &&
             !runtime.trace().empty(),
         "persistent values, timers, and opt-in trace should update on commit");
  const auto trace_json = export_layerc_trace(runtime.trace());
  const auto parsed_trace = parse_layerc_trace(trace_json);
  const auto *trace_events =
      std::get_if<std::vector<AutomationEvent>>(&parsed_trace);
  expect(trace_events != nullptr && !trace_events->empty() &&
             trace_events->front().kind == event.kind &&
             trace_events->front().source_id == event.source_id &&
             trace_events->front().fields.at("confidence") ==
                 AutomationValue(ConfidenceValue{0.75}) &&
             trace_events->front().fields.at("delay") ==
                 AutomationValue(DurationValue{25ms}),
         "saved traces should round trip typed replayable source events");

  std::vector<AutomationTraceEntry> large_trace;
  large_trace.reserve(2048U);
  for (std::size_t index{}; index < 2048U; ++index) {
    AutomationEvent source{AutomationEventKind::input_value_changed,
                           TimePoint(Milliseconds(index)),
                           std::string(max_automation_string_bytes, 's'),
                           static_cast<std::uint64_t>(index + 1U)};
    source.fields.emplace(
        std::string(max_automation_string_bytes, 'f'),
        AutomationValue(std::string(max_automation_string_bytes, 'v')));
    large_trace.push_back({.sequence = index + 1U,
                           .timestamp = Milliseconds(index),
                           .phase = "source",
                           .detail = "inputValueChanged",
                           .source_event = std::move(source)});
  }
  LiveMessage trace_message;
  trace_message.type = LiveMessageType::automation_trace_event;
  trace_message.profile_key = "game.json";
  const auto trace_prefix = live_trace_prefix_count(trace_message, large_trace);
  expect(trace_prefix > 0U && trace_prefix < large_trace.size(),
         "large traces should be split into nonempty live-frame chunks");
  if (trace_prefix > 0U) {
    trace_message.text =
        export_layerc_trace(std::span<const AutomationTraceEntry>(
            large_trace.data(), trace_prefix));
    expect(std::holds_alternative<std::vector<std::byte>>(
               encode_live_frame(trace_message)),
           "each selected trace chunk must fit the 512 KiB live-frame cap");
  }
  AutomationProfile immutable;
  immutable.profile_id = "game";
  immutable.variables = {{.id = "value",
                          .type = AutomationValueType::integer,
                          .default_value = AutomationValue(std::int64_t{0})},
                         {.id = "observed",
                          .type = AutomationValueType::boolean,
                          .default_value = AutomationValue(false)}};
  AutomationRule increment{
      .id = "increment",
      .priority = 10,
      .event = AutomationEventKind::input_pressed,
      .source_filter = "key-e",
      .actions = {{.kind = AutomationActionKind::increment_variable,
                   .target = "value",
                   .value = literal(AutomationValue(std::int64_t{1}))}}};
  AutomationRule observe{
      .id = "observe",
      .event = AutomationEventKind::input_pressed,
      .source_filter = "key-e",
      .condition = {.kind = ExpressionKind::binary,
                    .operation = "greater",
                    .arguments = {variable("value"),
                                  literal(AutomationValue(std::int64_t{0}))}},
      .actions = {{.kind = AutomationActionKind::set_variable,
                   .target = "observed",
                   .value = literal(AutomationValue(true))}}};
  immutable.automations = {{.id = "immutable-root",
                            .name = "Immutable root",
                            .rules = {increment, observe}}};
  AutomationRuntime immutable_runtime(immutable);
  const auto immutable_first = immutable_runtime.process(event);
  expect(immutable_first.committed &&
             immutable_runtime.snapshot().variables.at("value") ==
                 AutomationValue(std::int64_t{1}) &&
             immutable_runtime.snapshot().variables.at("observed") ==
                 AutomationValue(false),
         "all conditions in one root transaction should read one immutable "
         "pre-event snapshot");
  const auto immutable_second = immutable_runtime.process(event);
  expect(immutable_second.committed &&
             immutable_runtime.snapshot().variables.at("observed") ==
                 AutomationValue(true),
         "a later root event should observe the previously committed snapshot");

  AutomationRuntime rejected(profile);
  rejected.set_trace_enabled(true);
  const auto rolled_back =
      rejected.process(event, [](const AutomationEffect &effect) {
        return effect.kind != AutomationActionKind::trigger_cooldown;
      });
  expect(
      rolled_back.committed &&
          std::ranges::find(rolled_back.faulted_automations, "basic") !=
              rolled_back.faulted_automations.end() &&
          rejected.snapshot().variables.at("count") ==
              AutomationValue(std::int64_t{0}) &&
          rejected.snapshot().timers.empty(),
      "a rejected effect should fault its automation, roll back its effects, "
      "and complete the root transaction");

  const auto rejected_trace_json = export_layerc_trace(rejected.trace());
  const auto rejected_trace = parse_layerc_trace(rejected_trace_json);
  const auto *rejected_events =
      std::get_if<std::vector<AutomationEvent>>(&rejected_trace);
  expect(rejected_events != nullptr && rejected_events->size() == 1U,
         "fault isolation retries should record a root source event once");
  const std::vector<AutomationEvent> events{event, event};
  const std::vector<AutomationReplayAssertion> assertions{
      {.variable_id = "count", .expected = AutomationValue(std::int64_t{2})}};
  const auto replay = replay_automation_events(profile, events, assertions);
  expect(replay.passed && replay.processed_events == 2,
         "deterministic replay should use the production evaluator");

  auto propagation_base = automation_base();
  propagation_base.cooldowns.push_back(
      {.id = "blink", .name = "Blink", .duration = 5s});
  propagation_base.cooldowns.push_back(
      {.id = "roll", .name = "Roll", .duration = 5s});
  propagation_base.cooldown_groups = {
      {.id = "first",
       .name = "First",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"dash", "blink"}},
      {.id = "second",
       .name = "Second",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"blink", "roll"}}};
  for (auto &cooldown : propagation_base.cooldowns) {
    cooldown.max_charges = 2;
    cooldown.recovery = RecoveryMode::sequential;
  }
  CooldownEngine propagated_cooldowns(propagation_base.cooldowns,
                                      propagation_base.cooldown_groups);
  AutomationEffect trigger_effect{
      .kind = AutomationActionKind::trigger_cooldown, .target = "dash"};
  expect(apply_automation_cooldown_effect(propagated_cooldowns, trigger_effect,
                                          TimePoint{}, 77U),
         "the daemon cooldown adapter should accept triggerCooldown");
  auto propagated_states = propagated_cooldowns.snapshot(TimePoint{});
  expect(std::ranges::all_of(
             propagated_states,
             [](const auto &state) { return state.available_charges == 1U; }),
         "committed triggerCooldown should trigger every propagation member");
  (void)apply_automation_cooldown_effect(propagated_cooldowns, trigger_effect,
                                         TimePoint{}, 77U);
  propagated_states = propagated_cooldowns.snapshot(TimePoint{});
  expect(std::ranges::all_of(
             propagated_states,
             [](const auto &state) { return state.available_charges == 1U; }),
         "one origin token must not trigger a propagation fan twice");
  (void)apply_automation_cooldown_effect(propagated_cooldowns, trigger_effect,
                                         TimePoint{}, 78U);
  propagated_states = propagated_cooldowns.snapshot(TimePoint{});
  expect(std::ranges::all_of(
             propagated_states,
             [](const auto &state) { return state.available_charges == 0U; }),
         "a later root origin should be allowed to trigger the fan again");
  auto propagation_profile = automation_fixture();
  for (const auto *id : {"dashSeen", "blinkSeen", "rollSeen"})
    propagation_profile.variables.push_back(
        {.id = id,
         .type = AutomationValueType::integer,
         .default_value = AutomationValue(std::int64_t{0})});
  for (const auto &[source, target] :
       std::array<std::pair<const char *, const char *>, 3>{
           {{"dash", "dashSeen"},
            {"blink", "blinkSeen"},
            {"roll", "rollSeen"}}}) {
    propagation_profile.automations.front().rules.push_back(
        {.id = std::string("observe-") + source,
         .event = AutomationEventKind::cooldown_triggered,
         .source_filter = source,
         .actions = {{.kind = AutomationActionKind::increment_variable,
                      .target = target,
                      .value = literal(AutomationValue(std::int64_t{1}))}}});
  }
  AutomationRuntime propagation_runtime(propagation_profile, {},
                                        propagation_base.cooldown_groups);
  const auto propagated = propagation_runtime.process(event);
  expect(propagated.committed &&
             propagation_runtime.snapshot().variables.at("dashSeen") ==
                 AutomationValue(std::int64_t{1}) &&
             propagation_runtime.snapshot().variables.at("blinkSeen") ==
                 AutomationValue(std::int64_t{1}) &&
             propagation_runtime.snapshot().variables.at("rollSeen") ==
                 AutomationValue(std::int64_t{1}),
         "automation-derived cooldown triggers should fan through overlapping "
         "propagation groups within the same bounded root transaction");
}

void test_automation_priority_fault_isolation_and_reconfigure() {
  using namespace layerc::core;
  auto profile = automation_fixture();
  auto lower = profile.automations.front();
  lower.id = "lower";
  lower.name = "Lower";
  lower.rules.front().id = "lower-rule";
  lower.rules.front().priority = 1;
  lower.rules.front().actions = {
      {.kind = AutomationActionKind::set_variable,
       .target = "count",
       .value = literal(AutomationValue(std::int64_t{99}))}};
  profile.automations.push_back(lower);

  AutomationRuntime runtime(profile);
  const AutomationEvent event{AutomationEventKind::input_pressed, TimePoint{},
                              "key-e", 1};
  const auto result = runtime.process(event);
  expect(result.committed && runtime.snapshot().variables.at("count") ==
                                 AutomationValue(std::int64_t{1}),
         "the higher-priority action should win a per-target conflict");

  auto changed = profile;
  changed.automations.front().name = "Changed";
  runtime.reconfigure(changed);
  expect(runtime.snapshot().timers.empty(),
         "editing an automation should cancel timers owned by that automation");

  AutomationProfile isolated;
  isolated.profile_id = "game";
  isolated.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})});
  AutomationRule broken{
      .id = "broken",
      .priority = 20,
      .event = AutomationEventKind::session_started,
      .actions = {{.kind = AutomationActionKind::set_variable,
                   .target = "count",
                   .value = {.kind = ExpressionKind::binary,
                             .operation = "divide",
                             .arguments = {literal(AutomationValue(1.0)),
                                           literal(AutomationValue(0.0))}}}}};
  AutomationRule healthy{
      .id = "healthy",
      .event = AutomationEventKind::session_started,
      .actions = {{.kind = AutomationActionKind::increment_variable,
                   .target = "count",
                   .value = literal(AutomationValue(std::int64_t{1}))}}};
  isolated.automations = {{.id = "bad", .name = "Bad", .rules = {broken}},
                          {.id = "good", .name = "Good", .rules = {healthy}}};
  AutomationRuntime fault_runtime(isolated);
  const auto fault = fault_runtime.process(
      {AutomationEventKind::session_started, TimePoint{}, "", 2});
  expect(fault.committed &&
             std::ranges::find(fault.faulted_automations, "bad") !=
                 fault.faulted_automations.end() &&
             fault_runtime.snapshot().variables.at("count") ==
                 AutomationValue(std::int64_t{1}),
         "a runtime error should fault only the offending automation");
}

void test_automation_persistent_sync_and_cascade_bounds() {
  using namespace layerc::core;

  // A sibling persistent-value push is not a durable write of THIS runtime's
  // snapshot and must not clear the unsaved-edit marker.
  const auto profile = automation_fixture();
  AutomationRuntime runtime(profile);
  (void)runtime.process(
      {AutomationEventKind::input_pressed, TimePoint{}, "key-e", 1});
  expect(runtime.persistent_dirty(),
         "committing a persistent change should mark the runtime dirty");
  runtime.apply_persistent_values(runtime.snapshot().persistent_values);
  expect(runtime.persistent_dirty(),
         "a sibling persistent push must not clear the dirty flag");
  runtime.mark_persistent_saved();
  expect(!runtime.persistent_dirty(),
         "acknowledging a durable write should clear the dirty flag");

  // A profile switch must re-baseline the daemon edge monitors with it.
  auto &edges = runtime.edges();
  edges.cooldown_ready["dash"] = true;
  edges.group_ready["movement"] = true;
  edges.input_values["controller:1:0"] = 0.5;
  auto switched = profile;
  switched.automations.front().name = "Switched";
  runtime.reconfigure(switched);
  expect(runtime.edges().cooldown_ready.empty() &&
             runtime.edges().group_ready.empty() &&
             runtime.edges().input_values.empty() &&
             runtime.edges().input_emitted.empty(),
         "reconfigure should reset the session edge monitors atomically");

  // The fault-retry cascade restarts under one shared deadline: every faulted
  // automation is recorded in order while healthy automations still commit.
  AutomationProfile cascade;
  cascade.profile_id = "game";
  cascade.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})});
  const auto broken_rule = [](const std::string &id) {
    AutomationRule rule;
    rule.id = id;
    rule.event = AutomationEventKind::session_started;
    rule.condition = {.kind = ExpressionKind::variable,
                      .reference = "missing-" + id};
    rule.actions = {{.kind = AutomationActionKind::increment_variable,
                     .target = "count",
                     .value = literal(AutomationValue(std::int64_t{100}))}};
    return rule;
  };
  AutomationRule healthy{
      .id = "healthy",
      .event = AutomationEventKind::session_started,
      .actions = {{.kind = AutomationActionKind::increment_variable,
                   .target = "count",
                   .value = literal(AutomationValue(std::int64_t{1}))}}};
  cascade.automations = {
      {.id = "broken-1", .name = "B1", .rules = {broken_rule("one")}},
      {.id = "broken-2", .name = "B2", .rules = {broken_rule("two")}},
      {.id = "broken-3", .name = "B3", .rules = {broken_rule("three")}},
      {.id = "healthy-root", .name = "Healthy", .rules = {healthy}}};
  AutomationRuntime cascade_runtime(cascade);
  const auto cascade_result = cascade_runtime.process(
      {AutomationEventKind::session_started, TimePoint{}, "", 3});
  expect(cascade_result.committed &&
             cascade_result.faulted_automations ==
                 std::vector<std::string>{"broken-1", "broken-2", "broken-3"} &&
             cascade_runtime.snapshot().variables.at("count") ==
                 AutomationValue(std::int64_t{1}) &&
             cascade_result.node_evaluations > 0 &&
             cascade_result.error.has_value() &&
             cascade_result.error->message.find("missing-three") !=
                 std::string::npos,
         "one deadline should drive the fault cascade while healthy "
         "automations commit after the faults");

  // Exceeding the derived-event cap must fault only the offending automation
  // while the rest of the transaction commits (the queue-level guard is
  // defense in depth and must reject rather than commit if ever reached).
  // One triggerGroup action over a 33-member propagation group derives 34
  // events in a single rule, so the cap trips deterministically without
  // racing the 2 ms execution budget.
  AutomationProfile fanout;
  fanout.profile_id = "game";
  fanout.variables.push_back(
      {.id = "marker",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})});
  AutomationRule marker_rule{
      .id = "marker",
      .priority = 100,
      .event = AutomationEventKind::session_started,
      .actions = {{.kind = AutomationActionKind::set_variable,
                   .target = "marker",
                   .value = literal(AutomationValue(std::int64_t{1}))}}};
  AutomationRule fan_rule{
      .id = "fan",
      .event = AutomationEventKind::session_started,
      .actions = {{.kind = AutomationActionKind::trigger_group,
                   .target = "burst"}}};
  std::vector<std::string> burst_members;
  for (std::size_t index{}; index <= max_automation_derived_events; ++index)
    burst_members.push_back("c" + std::to_string(index));
  const std::vector<CooldownGroup> burst_groups{
      {.id = "burst",
       .name = "Burst",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = burst_members}};
  fanout.automations = {{.id = "keeper", .name = "Keeper", .rules = {marker_rule}},
                        {.id = "fanout", .name = "Fanout", .rules = {fan_rule}}};
  AutomationRuntime fanout_runtime(fanout, {}, burst_groups);
  const auto fanout_result = fanout_runtime.process(
      {AutomationEventKind::session_started, TimePoint{}, "", 4});
  expect(fanout_result.committed &&
             fanout_result.faulted_automations ==
                 std::vector<std::string>{"fanout"} &&
             fanout_result.error.has_value() &&
             fanout_result.error->message.find("derived events") !=
                 std::string::npos &&
             fanout_runtime.snapshot().variables.at("marker") ==
                 AutomationValue(std::int64_t{1}) &&
             fanout_result.derived_events.size() == 1 &&
             fanout_result.derived_events.front().kind ==
                 AutomationEventKind::variable_changed,
         "the derived-event cap should fault only the offending automation "
         "while the healthy transaction commits");
}

} // namespace layerc::test