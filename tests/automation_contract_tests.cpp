#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/automation_session.hpp"
#include "layerc/core/cooldown_engine.hpp"

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

using expr::binary;
using expr::literal;
AutomationExpression unary(std::string operation,
                           AutomationExpression argument) {
  return {.kind = ExpressionKind::unary,
          .operation = std::move(operation),
          .arguments = {std::move(argument)}};
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
  // Operator contracts must not depend on the CI host's scheduling latency.
  AutomationRuntime runtime(profile, {}, {}, [] { return TimePoint{}; });
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
  {
    auto base = contract_base();
    base.cooldown_groups.back().duration = 5s;
    CooldownEngine engine(base.cooldowns, base.cooldown_groups);
    const std::array effects{
        AutomationEffect{.kind = AutomationActionKind::trigger_cooldown, .target = "dash"},
        AutomationEffect{.kind = AutomationActionKind::reduce_cooldown_next, .target = "blink"}};
    expect(!commit_automation_cooldown_effects(
               engine, effects, base.cooldowns, base.cooldown_groups, TimePoint{}, 1),
           "a rejected later effect must leave all cooldowns untouched");
    expect(engine.remaining("dash", TimePoint{}) == 0ms,
           "failed atomic effect commit must not consume an earlier charge");
    expect(!validate_automation_cooldown_effect(
               AutomationEffect{.kind = AutomationActionKind::set_overlay_override},
               base.cooldowns, base.cooldown_groups),
           "cooldown validation must reject effects from other domains");
    AutomationProfile graph;
    graph.profile_id = base.id;
    AutomationRuntime runtime(graph);
    expect(!next_automation_deadline(runtime, {}).has_value(),
           "idle automation must have no deadline");
    const std::array states{CooldownState{.next_charge_at = TimePoint{} + 100ms}};
    expect(next_automation_deadline(runtime, states, TimePoint{} + 200ms) ==
               TimePoint{} + 100ms,
           "charge recovery must precede a later checkpoint");
    expect(next_automation_deadline(runtime, states, TimePoint{} + 50ms) ==
               TimePoint{} + 50ms,
           "an earlier checkpoint must precede charge recovery");
    graph.automations = {{.id = "overlay", .name = "Overlay",
      .rules = {{.id = "start", .event = AutomationEventKind::session_started,
        .actions = {{.kind = AutomationActionKind::set_overlay_override,
          .target = "main", .name = "temporary", .property = "visible",
          .value = literal(AutomationValue(false)),
          .secondary = literal(AutomationValue(DurationValue{25ms}))}}}}}};
    AutomationRuntime overlay_runtime(graph);
    expect(overlay_runtime.process({.kind = AutomationEventKind::session_started}).committed,
           "overlay expiry fixture should commit");
    expect(next_automation_deadline(overlay_runtime, states) == TimePoint{} + 25ms,
           "overlay expiry must schedule a wakeup without any runtime timer");
  }

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
             action_runtime.next_timer_due() == TimePoint(50ms),
         "collection must be pure: the firing is consumed by process(), so "
         "the timer stays due until its event is delivered");
  const auto cleaned = action_runtime.process(timer_events.front());
  expect(cleaned.committed && action_runtime.snapshot().timers.empty() &&
             action_runtime.snapshot().overlay_overrides.empty() &&
             action_runtime.snapshot().variables.at("record") ==
                 AutomationValue(AutomationMap{}),
         "erase, cancel, and explicit override-clear actions should commit");

  {
    AutomationProfile rearm_profile;
    rearm_profile.profile_id = "game";
    AutomationRule arm;
    arm.id = "arm";
    arm.event = AutomationEventKind::session_started;
    arm.actions = {{.kind = AutomationActionKind::create_timer,
                    .target = "tick",
                    .value = literal(AutomationValue(DurationValue{50ms})),
                    .repeating = true}};
    rearm_profile.automations = {
        {.id = "clock", .name = "Clock", .rules = {arm}}};
    AutomationRuntime rearm(rearm_profile);
    (void)rearm.process({.kind = AutomationEventKind::session_started,
                         .timestamp = TimePoint{},
                         .origin_token = 1});
    const auto firing = rearm.due_timer_events(TimePoint(120ms));
    const auto fired = rearm.process(firing.front());
    expect(firing.size() == 1U && fired.committed &&
               rearm.next_timer_due() == TimePoint(170ms),
           "a repeating timer firing should rearm from the event timestamp "
           "when process() consumes it");
    const auto stale = rearm.process(
        {.kind = AutomationEventKind::timer_fired,
         .timestamp = TimePoint(121ms),
         .source_id = "tick",
         .origin_token = 2});
    expect(stale.committed && rearm.next_timer_due() == TimePoint(170ms),
           "a redelivered firing before the rearmed deadline must not "
           "consume the timer again");
  }

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

void test_automation_timer_commit_rollback() {
  for (const bool repeating : {false, true}) {
    for (const bool with_actions : {false, true}) {
      AutomationProfile profile;
      profile.profile_id = "game";
      profile.variables = {{.id = "count",
                            .type = AutomationValueType::integer,
                            .default_value = AutomationValue(std::int64_t{0}),
                            .persistent = true}};
      AutomationRule arm{
          .id = "arm",
          .event = AutomationEventKind::session_started,
          .actions = {{.kind = AutomationActionKind::create_timer,
                       .target = "tick",
                       .value = literal(AutomationValue(DurationValue{50ms})),
                       .repeating = repeating}}};
      profile.automations = {{.id = "clock", .name = "Clock", .rules = {arm}}};
      if (with_actions)
        profile.automations.front().rules.push_back(
            {.id = "tick",
             .event = AutomationEventKind::timer_fired,
             .source_filter = "tick",
             .actions = {{.kind = AutomationActionKind::increment_variable,
                          .target = "count",
                          .value = literal(AutomationValue(std::int64_t{1}))},
                         {.kind = AutomationActionKind::trigger_cooldown,
                          .target = "dash"}}});
      expect(validate_automation_profile(profile, contract_base())
                 .message.empty(),
             "timer rollback fixture should validate");
      AutomationRuntime runtime(profile);
      const auto armed = runtime.process(
          {.kind = AutomationEventKind::session_started});
      expect(armed.committed,
             "timer rollback fixture should arm its timer");
      runtime.mark_persistent_saved();
      const auto before = runtime.snapshot();
      const auto due = runtime.due_timer_events(TimePoint(120ms));
      expect(due.size() == 1U, "the timer should be collected once when late");
      if (due.size() != 1U)
        continue;
      const auto unchanged = [&] {
        const auto &snapshot = runtime.snapshot();
        return snapshot.timers.size() == 1U &&
               snapshot.timers.front().id == "tick" &&
               snapshot.timers.front().due == TimePoint(50ms) &&
               snapshot.timers.front().interval == 50ms &&
               snapshot.timers.front().repeating == repeating &&
               snapshot.variables == before.variables &&
               snapshot.persistent_values == before.persistent_values &&
               !runtime.persistent_dirty();
      };
      unsigned commit_calls{};
      const auto rejected = runtime.process(
          due.front(), {}, [&](const std::span<const AutomationEffect> effects) {
            ++commit_calls;
            expect(unchanged(),
                   "the committer must observe unmodified live state");
            expect(effects.size() == (with_actions ? 2U : 0U),
                   "the committer should receive the complete winning set");
            return false;
          });
      expect(commit_calls == 1U && !rejected.committed && rejected.error &&
                 rejected.derived_events.empty() && unchanged(),
             "rejected firings must preserve timer and persistent state");
      expect(runtime.next_timer_due() == TimePoint(50ms) &&
                 runtime.due_timer_events(TimePoint(120ms)).size() == 1U,
             "a rejected firing must remain scheduled for redelivery");
      const auto accepted = runtime.process(
          due.front(), {}, [&](const std::span<const AutomationEffect>) {
            expect(unchanged(), "retry must stage changes until commit accepts");
            return true;
          });
      expect(accepted.committed && !accepted.error &&
                 runtime.snapshot().variables.at("count") ==
                     AutomationValue(std::int64_t{with_actions ? 1 : 0}) &&
                 runtime.persistent_dirty() == with_actions,
             "redelivery should commit the effects exactly once");
      expect(repeating ? runtime.next_timer_due() == TimePoint(170ms)
                       : runtime.snapshot().timers.empty(),
             "accepted firings retire one-shots and rearm repeats from "
             "event time");
    }
  }

  AutomationProfile full;
  full.profile_id = "game";
  AutomationRule arm{.id = "arm", .event = AutomationEventKind::session_started};
  for (std::size_t i{}; i < max_automation_timers; ++i)
    arm.actions.push_back(
        {.kind = AutomationActionKind::create_timer,
         .target = "tick-" + std::to_string(i),
         .value = literal(AutomationValue(DurationValue{50ms}))});
  const AutomationRule replace{
      .id = "replace",
      .event = AutomationEventKind::timer_fired,
      .source_filter = "tick-0",
      .actions = {{.kind = AutomationActionKind::create_timer,
                   .target = "replacement",
                   .value = literal(AutomationValue(DurationValue{100ms}))}}};
  full.automations = {
      {.id = "clock", .name = "Clock", .rules = {arm, replace}}};
  expect(validate_automation_profile(full, contract_base()).message.empty(),
         "full timer fixture should validate");
  // Capacity is a functional boundary, not an execution-time benchmark.
  AutomationRuntime runtime(full, {}, {}, [] { return TimePoint{}; });
  const auto armed = runtime.process(
      {.kind = AutomationEventKind::session_started});
  expect(armed.committed, "the timer capacity fixture should arm all timers");
  const auto replaced = runtime.process({.kind = AutomationEventKind::timer_fired,
                                        .timestamp = TimePoint(50ms),
                                        .source_id = "tick-0"});
  const auto &timers = runtime.snapshot().timers;
  expect(replaced.committed && replaced.faulted_automations.empty() &&
             timers.size() == max_automation_timers &&
             std::ranges::find(timers, "tick-0", &AutomationTimer::id) ==
                 timers.end() &&
             std::ranges::find(timers, "replacement", &AutomationTimer::id) !=
                 timers.end(),
         "a staged one-shot firing must free capacity before evaluating "
         "its actions");
}

void test_automation_action_value_type() {
  AutomationProfile profile;
  profile.variables = {{.id = "value", .type = AutomationValueType::integer}};
  using Type = AutomationValueType;
  const auto expected = std::to_array<std::optional<Type>>({
      Type::integer, Type::integer, std::nullopt, std::nullopt,
      Type::string, std::nullopt, std::nullopt, Type::duration,
      Type::duration, std::nullopt, std::nullopt, std::nullopt,
      Type::duration, std::nullopt, Type::boolean, std::nullopt});
  static_assert(expected.size() == automation_action_kind_count);
  for (const auto &spec : automation_action_specs) {
    const AutomationAction action{.kind = spec.kind,
                                  .target = "value", .property = "visible"};
    expect(automation_action_value_type(action, profile) ==
               expected[static_cast<std::size_t>(spec.kind)],
           "each action should retain its literal editing type");
  }
  for (const auto kind : {AutomationActionKind::set_variable,
                          AutomationActionKind::increment_variable}) {
    const AutomationAction action{.kind = kind, .target = "value"};
    for (const auto &type : automation_value_type_wire) {
      profile.variables.front().type = type.value;
      expect(automation_action_value_type(action, profile) == type.value,
             "variable value hints must follow the declaration, not the default");
    }
    expect(!automation_action_value_type(
               {.kind = kind, .target = "missing"}, profile),
           "an unknown variable has no literal type hint");
  }
  for (const auto property : {"visible", "showText", "hideWhenReady", "text",
                              "layout", "width", "height", "fillColor",
                              "backgroundColor", "borderColor", "textColor",
                              "anchorX", "anchorY"}) {
    const auto *spec = find_overlay_property(property);
    expect(spec != nullptr &&
               automation_action_value_type(
                   {.kind = AutomationActionKind::set_overlay_override,
                    .property = property}, profile) == spec->type,
           "overlay hints must use the canonical property metadata");
  }
  expect(automation_action_value_type(
             {.kind = AutomationActionKind::set_overlay_override,
              .property = "unknown"}, profile) == Type::integer,
         "unknown overlay properties retain the editor's integer placeholder");
  expect(!automation_action_value_type(
             {.kind = static_cast<AutomationActionKind>(
                  automation_action_kind_count)}, profile),
         "unknown action kinds have no literal type hint");
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
  AutomationRuntime timer_runtime(timer_profile, {}, {},
                                  [] { return TimePoint{}; });
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

void test_automation_root_budget_isolation() {
  AutomationProfile profile;
  profile.profile_id = "game";
  const AutomationRule rule{
      .id = "reset",
      .event = AutomationEventKind::session_started,
      .actions = {
          {.kind = AutomationActionKind::reset_cooldown, .target = "dash"}}};
  profile.automations = {
      {.id = "owner", .name = "Owner", .rules = {rule}},
      {.id = "untouched", .name = "Untouched", .rules = {rule}}};
  expect(validate_automation_profile(profile, contract_base()).message.empty(),
         "AR-01 budget fixture must be valid");
  TimePoint now{};
  const auto clock = [&] { return now; };
  const AutomationEvent root{.kind = AutomationEventKind::session_started};
  unsigned commits{};
  AutomationRuntime rejected(profile, {}, {}, clock);
  const auto failure = rejected.process(
      root,
      [&](const AutomationEffect &) {
        now += 3ms;
        return false; // attributable rejection after the shared budget expires
      },
      [&](std::span<const AutomationEffect>) {
        ++commits;
        return true;
      });
  expect(!failure.committed && failure.error && commits == 0U &&
             failure.faulted_automations == std::vector<std::string>{"owner"},
         "AR-01 expired retries must not fault untouched automations");
  const auto recovered = rejected.process(root);
  expect(
      recovered.committed && recovered.effects.size() == 1U &&
          recovered.effects.front().automation_id == "untouched",
      "AR-01 the untouched automation must run on the next root without retry");

  AutomationRuntime late(profile, {}, {}, clock);
  late.set_trace_enabled(true);
  const auto expired = late.process(
      root,
      [&](const AutomationEffect &) {
        now += 3ms;
        return true;
      },
      [&](std::span<const AutomationEffect>) {
        ++commits;
        return true;
      });
  expect(!expired.committed && expired.error && expired.effects.empty() &&
             expired.faulted_automations.empty() && commits == 0U,
         "AR-01 expiration during the last validator must prevent external "
         "commit");
  expect(std::ranges::count(late.trace(), std::string("root"),
                            &AutomationTraceEntry::phase) == 1,
         "AR-01 an exhausted root must still have one trace entry");
  expect(late.process(root).committed,
         "AR-01 budget exhaustion must not permanently disable any automation");

  AutomationProfile nodes;
  nodes.profile_id = "game";
  for (std::size_t i{}; i < 33U; ++i) {
    auto expensive = rule;
    expensive.source_filter = "stress";
    expensive.condition = boolean_tree(7U); // 255 nodes, under the rule cap
    nodes.automations.push_back({.id = "a" + std::to_string(i),
                                 .name = "Nodes",
                                 .rules = {std::move(expensive)}});
  }
  auto healthy = rule;
  healthy.source_filter = "healthy";
  nodes.automations.push_back(
      {.id = "healthy", .name = "Healthy", .rules = {healthy}});
  expect(validate_automation_profile(nodes, contract_base()).message.empty(),
         "AR-01 node exhaustion must be tested with a valid bounded graph");
  AutomationRuntime exhausted(nodes, {}, {}, clock);
  const auto capped =
      exhausted.process({.kind = root.kind, .source_id = "stress"});
  expect(!capped.committed && capped.error &&
             capped.faulted_automations.empty() &&
             capped.node_evaluations == max_automation_node_evaluations &&
             capped.error->message.find("8192") != std::string::npos,
         "AR-01 global node exhaustion must stop without a fault cascade");
  const auto next =
      exhausted.process({.kind = root.kind, .source_id = "healthy"});
  expect(next.committed && next.effects.size() == 1U,
         "AR-01 node exhaustion must leave untouched automations enabled");

  profile.automations.resize(1U);
  profile.automations.front().rules.front().condition = boolean_tree(7U);
  unsigned reads{};
  AutomationRuntime cooperative(
      profile, {}, {}, [&] { return TimePoint(++reads >= 20U ? 3ms : 0ms); });
  const auto interrupted = cooperative.process(root);
  expect(!interrupted.committed && interrupted.error &&
             interrupted.faulted_automations.empty() &&
             interrupted.node_evaluations > 0U &&
             interrupted.node_evaluations < 255U,
         "AR-01 cooperative checks must stop within expression evaluation");
}

} // namespace layerc::test