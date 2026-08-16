#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

AutomationExpression literal(AutomationValue value) {
  return {.kind = ExpressionKind::literal, .literal = std::move(value)};
}
AutomationExpression unary(std::string operation,
                           AutomationExpression argument) {
  return {.kind = ExpressionKind::unary,
          .operation = std::move(operation),
          .arguments = {std::move(argument)}};
}
AutomationExpression binary(std::string operation, AutomationExpression left,
                            AutomationExpression right) {
  return {.kind = ExpressionKind::binary,
          .operation = std::move(operation),
          .arguments = {std::move(left), std::move(right)}};
}
AutomationExpression call(std::string operation,
                          std::vector<AutomationExpression> arguments) {
  return {.kind = ExpressionKind::call,
          .operation = std::move(operation),
          .arguments = std::move(arguments)};
}
AutomationExpression index(AutomationExpression target,
                           AutomationExpression key) {
  return {.kind = ExpressionKind::index,
          .arguments = {std::move(target), std::move(key)}};
}
AutomationExpression event_field(std::string field) {
  return {.kind = ExpressionKind::event_field, .reference = std::move(field)};
}

GameProfile contract_base() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  for (const auto *id : {"dash", "blink", "roll", "shield", "heal", "ward"})
    profile.cooldowns.push_back({.id = id, .name = id, .duration = 5s});
  profile.cooldown_groups = {{.id = "movement",
                              .name = "Movement",
                              .mode = CooldownGroupMode::trigger_propagation,
                              .cooldown_ids = {"dash", "blink"}},
                             {.id = "defense",
                              .name = "Defense",
                              .mode = CooldownGroupMode::shared_pool,
                              .cooldown_ids = {"shield", "ward"}}};
  profile.panels.push_back(
      {.id = "main", .name = "Main", .style = PanelStyle::bar});
  return profile;
}

std::optional<AutomationValue> evaluate(AutomationExpression expression,
                                        const AutomationValueType result_type,
                                        AutomationValue default_value,
                                        AutomationEvent event = {}) {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.variables.push_back({.id = "result",
                               .type = result_type,
                               .default_value = std::move(default_value)});
  profile.automations.push_back(
      {.id = "operator",
       .name = "Operator",
       .rules = {{.id = "evaluate",
                  .event = event.kind,
                  .actions = {{.kind = AutomationActionKind::set_variable,
                               .target = "result",
                               .value = std::move(expression)}}}}});
  if (!validate_automation_profile(profile, contract_base()).message.empty())
    return std::nullopt;
  AutomationRuntime runtime(profile);
  const auto result = runtime.process(event);
  if (!result.committed)
    return std::nullopt;
  return runtime.snapshot().variables.at("result");
}

AutomationExpression boolean_tree(const std::size_t depth) {
  if (depth == 0)
    return literal(AutomationValue(true));
  return binary("and", boolean_tree(depth - 1U), boolean_tree(depth - 1U));
}

} // namespace

void test_automation_expression_and_action_contract() {
  const auto boolean = [&](AutomationExpression expression) {
    return evaluate(std::move(expression), AutomationValueType::boolean,
                    AutomationValue(false));
  };
  const auto integer = [&](AutomationExpression expression) {
    return evaluate(std::move(expression), AutomationValueType::integer,
                    AutomationValue(std::int64_t{0}));
  };
  const auto number_value = [&](AutomationExpression expression) {
    return evaluate(std::move(expression), AutomationValueType::number,
                    AutomationValue(0.0));
  };

  expect(
      boolean(unary("not", literal(AutomationValue(false)))) ==
              AutomationValue(true) &&
          integer(unary("negate", literal(AutomationValue(std::int64_t{5})))) ==
              AutomationValue(std::int64_t{-5}) &&
          integer(
              unary("size", literal(AutomationValue(std::string("abc"))))) ==
              AutomationValue(std::int64_t{3}),
      "all unary automation operators should evaluate with typed results");

  expect(
      boolean(binary("and", literal(AutomationValue(true)),
                     literal(AutomationValue(false)))) ==
              AutomationValue(false) &&
          boolean(binary("or", literal(AutomationValue(false)),
                         literal(AutomationValue(true)))) ==
              AutomationValue(true) &&
          boolean(binary("equal", literal(AutomationValue(std::int64_t{7})),
                         literal(AutomationValue(std::int64_t{7})))) ==
              AutomationValue(true) &&
          boolean(binary("notEqual", literal(AutomationValue(std::int64_t{7})),
                         literal(AutomationValue(std::int64_t{8})))) ==
              AutomationValue(true),
      "Boolean and equality operators should evaluate deterministically");

  expect(
      boolean(binary("less", literal(AutomationValue(std::int64_t{2})),
                     literal(AutomationValue(std::int64_t{3})))) ==
              AutomationValue(true) &&
          boolean(binary("lessEqual", literal(AutomationValue(std::int64_t{3})),
                         literal(AutomationValue(std::int64_t{3})))) ==
              AutomationValue(true) &&
          boolean(binary("greater", literal(AutomationValue(std::int64_t{4})),
                         literal(AutomationValue(std::int64_t{3})))) ==
              AutomationValue(true) &&
          boolean(binary("greaterEqual",
                         literal(AutomationValue(std::int64_t{4})),
                         literal(AutomationValue(std::int64_t{4})))) ==
              AutomationValue(true),
      "all ordered comparisons should evaluate without int64 precision loss");

  expect(
      integer(binary("add", literal(AutomationValue(std::int64_t{5})),
                     literal(AutomationValue(std::int64_t{2})))) ==
              AutomationValue(std::int64_t{7}) &&
          integer(binary("subtract", literal(AutomationValue(std::int64_t{5})),
                         literal(AutomationValue(std::int64_t{2})))) ==
              AutomationValue(std::int64_t{3}) &&
          integer(binary("multiply", literal(AutomationValue(std::int64_t{5})),
                         literal(AutomationValue(std::int64_t{2})))) ==
              AutomationValue(std::int64_t{10}) &&
          integer(binary("modulo", literal(AutomationValue(std::int64_t{5})),
                         literal(AutomationValue(std::int64_t{2})))) ==
              AutomationValue(std::int64_t{1}) &&
          number_value(binary("divide", literal(AutomationValue(5.0)),
                              literal(AutomationValue(2.0)))) ==
              AutomationValue(2.5),
      "all arithmetic operators should return their statically declared type");

  expect(number_value(call("min", {literal(AutomationValue(8.0)),
                                   literal(AutomationValue(2.0))})) ==
                 AutomationValue(2.0) &&
             number_value(call("max", {literal(AutomationValue(8.0)),
                                       literal(AutomationValue(2.0))})) ==
                 AutomationValue(8.0) &&
             number_value(call("clamp", {literal(AutomationValue(5.0)),
                                         literal(AutomationValue(0.0)),
                                         literal(AutomationValue(3.0))})) ==
                 AutomationValue(3.0),
         "min, max, and clamp should evaluate their bounded argument lists");

  AutomationMap first{{"score", AutomationValue(std::int64_t{1})},
                      {"active", AutomationValue(true)}};
  AutomationMap second{{"score", AutomationValue(std::int64_t{2})},
                       {"active", AutomationValue(false)}};
  const AutomationList records{AutomationValue(first), AutomationValue(second)};
  const auto projected =
      evaluate(call("map", {literal(AutomationValue(records)),
                            literal(AutomationValue(std::string("score")))}),
               AutomationValueType::list, AutomationValue(AutomationList{}));
  const auto filtered = evaluate(
      call("filter", {literal(AutomationValue(records)),
                      literal(AutomationValue(std::string("active")))}),
      AutomationValueType::list, AutomationValue(AutomationList{}));
  expect(
      projected == AutomationValue(
                       AutomationList{AutomationValue(std::int64_t{1}),
                                      AutomationValue(std::int64_t{2})}) &&
          filtered == AutomationValue(AutomationList{AutomationValue(first)}),
      "map and filter should perform bounded map-field projection/filtering");
  expect(
      boolean(
          call("any", {literal(AutomationValue(AutomationList{
                          AutomationValue(false), AutomationValue(true)}))})) ==
              AutomationValue(true) &&
          boolean(call("all",
                       {literal(AutomationValue(AutomationList{
                           AutomationValue(true), AutomationValue(true)}))})) ==
              AutomationValue(true),
      "any and all should reduce bounded lists by truthiness");

  expect(integer(index(literal(AutomationValue(
                           AutomationList{AutomationValue(std::int64_t{4}),
                                          AutomationValue(std::int64_t{9})})),
                       literal(AutomationValue(std::int64_t{1})))) ==
                 AutomationValue(std::int64_t{9}) &&
             integer(index(literal(AutomationValue(first)),
                           literal(AutomationValue(std::string("score"))))) ==
                 AutomationValue(std::int64_t{1}),
         "list and map indexing should return the selected typed value");

  AutomationEvent event;
  event.kind = AutomationEventKind::visual_observation;
  event.source_id = "locator";
  event.fields.emplace("ageMs", AutomationValue(DurationValue{25ms}));
  expect(evaluate(event_field("sourceId"), AutomationValueType::string,
                  AutomationValue(std::string{}),
                  event) == AutomationValue(std::string("locator")) &&
             evaluate(event_field("ageMs"), AutomationValueType::duration,
                      AutomationValue(DurationValue{}),
                      event) == AutomationValue(DurationValue{25ms}),
         "sourceId and recorded event age should be deterministic fields");

  auto action_profile = AutomationProfile{};
  action_profile.profile_id = "game";
  action_profile.variables = {
      {.id = "items",
       .type = AutomationValueType::list,
       .default_value = AutomationValue(AutomationList{})},
      {.id = "record",
       .type = AutomationValueType::map,
       .default_value = AutomationValue(AutomationMap{})},
      {.id = "amount",
       .type = AutomationValueType::number,
       .default_value = AutomationValue(1.0)},
      {.id = "delay",
       .type = AutomationValueType::duration,
       .default_value = AutomationValue(DurationValue{50ms})},
      {.id = "confidence",
       .type = AutomationValueType::confidence,
       .default_value = AutomationValue(ConfidenceValue{0.2})}};
  AutomationRule start;
  start.id = "start";
  start.event = AutomationEventKind::session_started;
  start.actions = {
      {.kind = AutomationActionKind::append_list,
       .target = "items",
       .value = literal(AutomationValue(std::string("one")))},
      {.kind = AutomationActionKind::set_map_entry,
       .target = "record",
       .value = literal(AutomationValue(std::int64_t{7})),
       .secondary = literal(AutomationValue(std::string("key")))},
      {.kind = AutomationActionKind::increment_variable,
       .target = "amount",
       .value = literal(AutomationValue(0.5))},
      {.kind = AutomationActionKind::increment_variable,
       .target = "delay",
       .value = literal(AutomationValue(DurationValue{25ms}))},
      {.kind = AutomationActionKind::increment_variable,
       .target = "confidence",
       .value = literal(AutomationValue(ConfidenceValue{0.1}))},
      {.kind = AutomationActionKind::create_timer,
       .target = "pulse",
       .value = literal(AutomationValue(DurationValue{50ms})),
       .repeating = true},
      {.kind = AutomationActionKind::set_overlay_override,
       .target = "main",
       .name = "temporary",
       .property = "visible",
       .value = literal(AutomationValue(false)),
       .secondary = literal(AutomationValue(DurationValue{200ms}))}};
  AutomationRule cleanup;
  cleanup.id = "cleanup";
  cleanup.event = AutomationEventKind::timer_fired;
  cleanup.source_filter = "pulse";
  cleanup.actions = {
      {.kind = AutomationActionKind::erase_map_entry,
       .target = "record",
       .value = literal(AutomationValue(std::string("key")))},
      {.kind = AutomationActionKind::cancel_timer, .target = "pulse"},
      {.kind = AutomationActionKind::clear_overlay_override,
       .target = "temporary"}};
  action_profile.automations = {
      {.id = "state", .name = "State", .rules = {start, cleanup}}};
  expect(validate_automation_profile(action_profile, contract_base())
             .message.empty(),
         "the complete internal action fixture should validate");
  AutomationRuntime action_runtime(action_profile);
  const auto started =
      action_runtime.process({.kind = AutomationEventKind::session_started,
                              .timestamp = TimePoint{},
                              .origin_token = 1});
  expect(
      started.committed && action_runtime.snapshot().timers.size() == 1U &&
          action_runtime.snapshot().overlay_overrides.size() == 1U &&
          action_runtime.snapshot().variables.at("items") ==
              AutomationValue(
                  AutomationList{AutomationValue(std::string("one"))}) &&
          action_runtime.snapshot().variables.at("amount") ==
              AutomationValue(1.5) &&
          action_runtime.snapshot().variables.at("delay") ==
              AutomationValue(DurationValue{75ms}) &&
          std::abs(
              std::get<ConfidenceValue>(
                  action_runtime.snapshot().variables.at("confidence").storage)
                  .value -
              0.3) < 1e-12,
      "state, collection, timer, and overlay actions should commit together");
  expect(action_runtime.due_timer_events(TimePoint(49ms)).empty(),
         "monotonic timers should not fire before their deadline");
  const auto timer_events = action_runtime.due_timer_events(TimePoint(250ms));
  expect(timer_events.size() == 1U &&
             timer_events.front().source_id == "pulse" &&
             action_runtime.next_timer_due() == TimePoint(300ms),
         "repeating timers should coalesce sleep into one event and rearm from "
         "now");
  const auto cleaned = action_runtime.process(timer_events.front());
  expect(cleaned.committed && action_runtime.snapshot().timers.empty() &&
             action_runtime.snapshot().overlay_overrides.empty() &&
             action_runtime.snapshot().variables.at("record") ==
                 AutomationValue(AutomationMap{}),
         "erase, cancel, and explicit override-clear actions should commit");

  AutomationProfile external;
  external.profile_id = "game";
  external.automations.push_back(
      {.id = "cooldowns",
       .name = "Cooldowns",
       .rules = {{.id = "all",
                  .event = AutomationEventKind::input_pressed,
                  .actions = {
                      {.kind = AutomationActionKind::trigger_cooldown,
                       .target = "dash"},
                      {.kind = AutomationActionKind::reset_cooldown,
                       .target = "blink"},
                      {.kind = AutomationActionKind::reduce_cooldown_next,
                       .target = "roll",
                       .value = literal(AutomationValue(DurationValue{10ms}))},
                      {.kind = AutomationActionKind::reduce_cooldown_all,
                       .target = "shield",
                       .value = literal(AutomationValue(DurationValue{10ms}))},
                      {.kind = AutomationActionKind::refund_cooldown,
                       .target = "heal"},
                      {.kind = AutomationActionKind::trigger_group,
                       .target = "movement"},
                      {.kind = AutomationActionKind::reset_group,
                       .target = "defense"}}}}});
  expect(validate_automation_profile(external, contract_base()).message.empty(),
         "every cooldown/group action kind should validate");
  AutomationRuntime external_runtime(external, {},
                                     contract_base().cooldown_groups);
  const auto effects =
      external_runtime.process({.kind = AutomationEventKind::input_pressed,
                                .timestamp = TimePoint{},
                                .origin_token = 2});
  expect(effects.committed && effects.effects.size() == 7U,
         "every non-conflicting cooldown/group action should survive one "
         "transaction");
}

void test_automation_limits_and_fault_rollback() {
  const auto base = contract_base();
  AutomationProfile profile;
  profile.profile_id = "game";
  for (std::size_t index{}; index <= max_automation_variables; ++index)
    profile.variables.push_back({.id = "v" + std::to_string(index),
                                 .type = AutomationValueType::boolean,
                                 .default_value = AutomationValue(false)});
  expect(!validate_automation_profile(profile, base).message.empty(),
         "more than 64 automation variables should be rejected");

  profile = {};
  profile.profile_id = "game";
  AutomationDefinition many_rules{.id = "many", .name = "Many"};
  for (std::size_t index{}; index <= max_automation_rules; ++index)
    many_rules.rules.push_back(
        {.id = "r" + std::to_string(index),
         .event = AutomationEventKind::session_started,
         .actions = {{.kind = AutomationActionKind::trigger_cooldown,
                      .target = "dash"}}});
  profile.automations.push_back(std::move(many_rules));
  expect(!validate_automation_profile(profile, base).message.empty(),
         "more than 64 total automation rules should be rejected");

  profile = {};
  profile.profile_id = "game";
  AutomationRule too_many_actions;
  too_many_actions.id = "actions";
  too_many_actions.event = AutomationEventKind::session_started;
  too_many_actions.actions.assign(
      max_automation_actions_per_rule + 1U,
      {.kind = AutomationActionKind::trigger_cooldown, .target = "dash"});
  profile.automations.push_back(
      {.id = "many", .name = "Many", .rules = {too_many_actions}});
  expect(!validate_automation_profile(profile, base).message.empty(),
         "more than 32 actions in one rule should be rejected");

  profile.automations.front().rules.front().actions.resize(1U);
  profile.automations.front().rules.front().condition = boolean_tree(8U);
  expect(!validate_automation_profile(profile, base).message.empty(),
         "more than 256 AST nodes in one rule should be rejected");

  AutomationProfile timer_profile;
  timer_profile.profile_id = "game";
  AutomationRule fill;
  fill.id = "fill";
  fill.event = AutomationEventKind::session_started;
  for (std::size_t index{}; index < max_automation_timers; ++index)
    fill.actions.push_back(
        {.kind = AutomationActionKind::create_timer,
         .target = "timer-" + std::to_string(index),
         .value = literal(AutomationValue(DurationValue{50ms}))});
  AutomationRule overflow;
  overflow.id = "overflow";
  overflow.event = AutomationEventKind::input_pressed;
  overflow.actions.push_back(
      {.kind = AutomationActionKind::create_timer,
       .target = "timer-overflow",
       .value = literal(AutomationValue(DurationValue{50ms}))});
  timer_profile.automations = {
      {.id = "timers", .name = "Timers", .rules = {fill, overflow}}};
  AutomationRuntime timer_runtime(timer_profile);
  expect(timer_runtime
                 .process({.kind = AutomationEventKind::session_started,
                           .timestamp = TimePoint{},
                           .origin_token = 1})
                 .committed &&
             timer_runtime.snapshot().timers.size() == max_automation_timers,
         "the exact 32-timer session limit should be accepted");
  const auto overflowed =
      timer_runtime.process({.kind = AutomationEventKind::input_pressed,
                             .timestamp = TimePoint{},
                             .origin_token = 2});
  expect(overflowed.committed && !overflowed.faulted_automations.empty() &&
             timer_runtime.snapshot().timers.size() == max_automation_timers,
         "a 33rd timer should fault only its automation and preserve committed "
         "state");

  AutomationProfile rollback;
  rollback.profile_id = "game";
  rollback.variables = {{.id = "early",
                         .type = AutomationValueType::integer,
                         .default_value = AutomationValue(std::int64_t{0})},
                        {.id = "healthy",
                         .type = AutomationValueType::integer,
                         .default_value = AutomationValue(std::int64_t{0})}};
  rollback.automations = {
      {.id = "bad",
       .name = "Bad",
       .rules = {{.id = "early",
                  .priority = 20,
                  .event = AutomationEventKind::session_started,
                  .actions = {{.kind = AutomationActionKind::set_variable,
                               .target = "early",
                               .value =
                                   literal(AutomationValue(std::int64_t{1}))}}},
                 {.id = "late",
                  .priority = 10,
                  .event = AutomationEventKind::session_started,
                  .actions = {{.kind = AutomationActionKind::set_variable,
                               .target = "early",
                               .value = binary(
                                   "divide", literal(AutomationValue(1.0)),
                                   literal(AutomationValue(0.0)))}}}}},
      {.id = "good",
       .name = "Good",
       .rules = {{.id = "healthy",
                  .event = AutomationEventKind::session_started,
                  .actions = {
                      {.kind = AutomationActionKind::set_variable,
                       .target = "healthy",
                       .value = literal(AutomationValue(std::int64_t{1}))}}}}}};
  AutomationRuntime rollback_runtime(rollback);
  const auto isolated =
      rollback_runtime.process({.kind = AutomationEventKind::session_started,
                                .timestamp = TimePoint{},
                                .origin_token = 3});
  expect(isolated.committed &&
             std::ranges::find(isolated.faulted_automations, "bad") !=
                 isolated.faulted_automations.end() &&
             rollback_runtime.snapshot().variables.at("early") ==
                 AutomationValue(std::int64_t{0}) &&
             rollback_runtime.snapshot().variables.at("healthy") ==
                 AutomationValue(std::int64_t{1}),
         "faulting a later rule should roll back all effects from that "
         "automation only");

  AutomationProfile overflow_math;
  overflow_math.profile_id = "game";
  overflow_math.variables.push_back(
      {.id = "value",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})});
  overflow_math.automations.push_back(
      {.id = "overflow",
       .name = "Overflow",
       .rules = {
           {.id = "overflow",
            .event = AutomationEventKind::session_started,
            .actions = {{.kind = AutomationActionKind::set_variable,
                         .target = "value",
                         .value = binary(
                             "add",
                             literal(AutomationValue(
                                 std::numeric_limits<std::int64_t>::max())),
                             literal(AutomationValue(std::int64_t{1})))}}}}});
  AutomationRuntime math_runtime(overflow_math);
  const auto math_result =
      math_runtime.process({.kind = AutomationEventKind::session_started,
                            .timestamp = TimePoint{},
                            .origin_token = 4});
  expect(math_result.committed && !math_result.faulted_automations.empty() &&
             math_runtime.snapshot().variables.at("value") ==
                 AutomationValue(std::int64_t{0}),
         "integer overflow should fault cleanly without undefined behavior or "
         "state leakage");

  for (std::int64_t value = -100; value <= 100; ++value) {
    const auto actual = evaluate(
        binary("add", literal(AutomationValue(value)),
               literal(AutomationValue(std::int64_t{7}))),
        AutomationValueType::integer, AutomationValue(std::int64_t{0}));
    expect(actual == AutomationValue(value + 7),
           "bounded integer addition property should hold");
  }

  auto reconfigured = rollback;
  AutomationRuntime group_runtime(reconfigured, {}, base.cooldown_groups);
  auto changed_groups = base.cooldown_groups;
  changed_groups.front().cooldown_ids = {"dash", "roll"};
  group_runtime.reconfigure(reconfigured, changed_groups);
  expect(std::ranges::equal(group_runtime.cooldown_groups(), changed_groups),
         "runtime reconfiguration should refresh base cooldown-group metadata");
}

} // namespace layerc::test