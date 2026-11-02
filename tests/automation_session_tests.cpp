#include "core_test_cases.hpp"
#include "automation_test_support.hpp"
#include "test_support.hpp"

#include "layerc/core/automation_session.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

GameProfile session_base() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = 5s});
  return profile;
}

AutomationProfile counting_profile() {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0}),
       .persistent = true});
  AutomationExpression one;
  one.kind = ExpressionKind::literal;
  one.literal = AutomationValue(std::int64_t{1});
  AutomationRule rule;
  rule.id = "count-dash";
  rule.event = AutomationEventKind::cooldown_triggered;
  rule.source_filter = "dash";
  rule.actions.push_back(
      {.kind = AutomationActionKind::increment_variable,
       .target = "count",
       .value = one});
  profile.automations.push_back(
      {.id = "count", .name = "Count", .enabled = true, .rules = {rule}});
  return profile;
}

AutomationProfile axis_profile() {
  auto profile = counting_profile();
  profile.automations.front().rules.front().event =
      AutomationEventKind::input_value_changed;
  profile.automations.front().rules.front().source_filter =
      "controller:0:leftX";
  return profile;
}

double field(const AutomationEvent &event, const std::string &name) {
  const auto found = event.fields.find(name);
  const auto *number = found == event.fields.end()
                           ? nullptr
                           : std::get_if<double>(&found->second.storage);
  return number == nullptr ? -1.0 : *number;
}

InputMatcher dash_matcher(const InputToken key) {
  return InputMatcher(std::vector<InputBinding>{
      {.cooldown_id = "dash", .chord = InputChord{{key}}}});
}
} // namespace

// F04: two automations that separate a fault of one from a successful event
// handled by the other. "fault-a" divides by the divisor, so it faults while
// the divisor is zero and recovers once the session sets it.
AutomationProfile health_profile(const double divisor,
                                 const bool include_faulting) {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.variables = {
      {.id = "divisor",
       .type = AutomationValueType::number,
       .default_value = AutomationValue(divisor)},
      {.id = "ratio",
       .type = AutomationValueType::number,
       .default_value = AutomationValue(0.0)},
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})}};
  if (include_faulting)
    profile.automations.push_back(
        {.id = "fault-a",
         .name = "Fault A",
         .rules = {{.id = "divide",
                    .event = AutomationEventKind::session_started,
                    .actions = {{.kind = AutomationActionKind::set_variable,
                                 .target = "ratio",
                                 .value = expr::binary(
                                     "divide",
                                     expr::literal(AutomationValue(1.0)),
                                     expr::variable("divisor"))}}}}});
  profile.automations.push_back(
      {.id = "healthy-b",
       .name = "Healthy B",
       .rules = {{.id = "bump",
                  .event = AutomationEventKind::cooldown_triggered,
                  .source_filter = "dash",
                  .actions = {
                      {.kind = AutomationActionKind::increment_variable,
                       .target = "count",
                       .value = expr::literal(
                           AutomationValue(std::int64_t{1}))},
                      {.kind = AutomationActionKind::set_variable,
                       .target = "divisor",
                       .value = expr::literal(AutomationValue(1.0))}}},
                 {.id = "reset-divisor",
                  .event = AutomationEventKind::input_pressed,
                  .source_filter = "reset",
                  .actions = {{.kind = AutomationActionKind::set_variable,
                               .target = "divisor",
                               .value = expr::literal(
                                   AutomationValue(0.0))}}}}});
  return profile;
}

// Windows and Linux daemon sessions both compose core::AutomationSession,
// so this fixes the trigger policy for every platform in one place.
void test_automation_session_trigger_policy() {
  const auto base = session_base();
  const InputToken key{InputDevice::keyboard, 69, 0};
  const auto now = TimePoint{} + 1s;

  AutomationSession valid(base);
  auto valid_matcher = dash_matcher(key);
  const auto fed = valid.feed_input(valid_matcher, InputEvent{key, true}, now);
  expect(!valid.blocks_all_triggers() && fed.matched && fed.changed &&
             fed.triggered == std::vector<std::string>{"dash"},
         "a valid session should trigger bound direct input");

  AutomationSession blocked(base);
  blocked.set_invalid_visual_sidecar(true);
  auto blocked_matcher = dash_matcher(key);
  const auto gated =
      blocked.feed_input(blocked_matcher, InputEvent{key, true}, now);
  expect(blocked.blocks_all_triggers() && gated.matched && !gated.changed &&
             gated.triggered.empty(),
         "an invalid visual sidecar should match input without triggering");
  expect(blocked.trigger("dash", now).triggered.empty(),
         "an invalid visual sidecar should block explicit triggers");
  (void)blocked.snapshot(now);
  expect(blocked.snapshot_buffer().size() == 1 &&
             blocked.snapshot_buffer().front().available_charges == 1,
         "blocked triggers must not consume cooldown charges");

  blocked.set_invalid_visual_sidecar(false);
  expect(blocked.trigger("dash", now).triggered ==
             std::vector<std::string>{"dash"},
         "a repaired visual sidecar should restore triggers");
}

void test_automation_session_origin_and_persistence() {
  const auto base = session_base();
  const auto now = TimePoint{} + 1s;

  const auto first =
      AutomationSession::event(AutomationEventKind::session_started, {}, now);
  const auto second =
      AutomationSession::event(AutomationEventKind::session_started, {}, now);
  expect(second.origin_token > first.origin_token,
         "origin tokens should come from one process-wide sequence");

  AutomationSession left(base);
  AutomationSession right(base);
  (void)left.start(accept_automation(counting_profile(), base), {}, now);
  (void)right.start(accept_automation(counting_profile(), base), {}, now);
  expect(!left.sync_persistent_to(right),
         "an unchanged session should not sync persistent values");

  const auto triggered = left.trigger("dash", now);
  expect(triggered.triggered == std::vector<std::string>{"dash"} &&
             triggered.changed,
         "a triggered cooldown should run cooldown_triggered automations");
  expect(left.sync_persistent_to(right),
         "a dirty session should sync persistent values to a sibling");
  const auto values = right.runtime()->persistent_values();
  const auto count = values.find("count");
  expect(count != values.end() &&
             count->second == AutomationValue(std::int64_t{1}),
         "the sibling should receive the winning persistent value");
}

// Continuous input is filtered once, in core, for every platform: demand,
// deadzone, the per-source rate limit and the change threshold.
void test_automation_session_continuous_input() {
  const auto base = session_base();
  const std::string axis = "controller:0:leftX";
  auto now = TimePoint{} + 1s;
  AutomationSession session(base);
  expect(!session.input_value_event(axis, 0.5, now),
         "a session without automation should not emit input values");
  (void)session.start(accept_automation(axis_profile(), base), {}, now);

  expect(!session.input_value_event("controller:0:leftY", 0.9, now) &&
             !session.input_value_event(axis, std::nan(""), now),
         "undemanded sources and non-finite samples should be ignored");
  expect(!session.input_value_event(axis, 0.1, now),
         "the first sample should only seed the source");
  now += 20ms;
  const auto moved = session.input_value_event(axis, 0.5, now);
  expect(moved.has_value() &&
             moved->kind == AutomationEventKind::input_value_changed &&
             moved->source_id == axis && field(*moved, "previous") == 0.0 &&
             field(*moved, "value") == 0.5,
         "a change should report the deadzoned previous value and the new one");
  now += 5ms;
  expect(!session.input_value_event(axis, 0.9, now),
         "a source should emit at most max_events_per_second");
  now += 20ms;
  expect(!session.input_value_event(axis, 0.51, now),
         "changes under the threshold should be dropped");
  const auto momentary = session.input_value_event(axis, 0.3, now, true);
  expect(momentary.has_value() && field(*momentary, "previous") == 0.0 &&
             field(*momentary, "value") == 0.3,
         "a momentary sample should report from zero on every accepted sample");
}

// Rule source filters match these ids byte for byte, and stored automation
// profiles carry them, so the spellings are fixed.
void test_automation_input_source_ids() {
  expect(input_source_id({InputDevice::keyboard, 18, 0}) == "keyboard:18" &&
             input_source_id({InputDevice::mouse, 2, 0}) == "mouse:2" &&
             input_source_id({InputDevice::controller, 5, 3}) ==
                 "controller:5:3" &&
             input_source_id({InputDevice::generic_hid, 1, 0}) == "hid:1",
         "button sources should be <device>:<code>[:<controller index>]");
  expect(controller_axis_source_id(2, "leftX") == "controller:2:leftX" &&
             hid_source_id(4) == "hid:4",
         "analog sources should name the controller axis or HID binding");
  expect(source_filter_names_device("mouse:1", InputDevice::mouse) &&
             source_filter_names_device("mouse:", InputDevice::mouse) &&
             !source_filter_names_device("mouse", InputDevice::mouse) &&
             !source_filter_names_device("mousey:1", InputDevice::mouse) &&
             !source_filter_names_device("", InputDevice::controller) &&
             source_filter_names_device("hid:3", InputDevice::generic_hid),
         "a source filter should name a device only by its '<label>:' prefix");
}

// F04: the session reports the runtime's current faults, so retry,
// reconfiguration, stop/start and a replacement runtime cannot leave a stale
// fault line behind.
void test_automation_session_current_health() {
  const auto base = session_base();
  const auto now = TimePoint{} + 1s;
  const auto started = [&](const TimePoint at) {
    return AutomationSession::event(AutomationEventKind::session_started, {},
                                    at);
  };
  const auto reset = AutomationSession::event(
      AutomationEventKind::input_pressed, "reset", now);

  AutomationSession session(base);
  (void)session.start(accept_automation(health_profile(0.0, true), base), {},
                      now);
  const auto fault_lines = session.health().lines("game.json");
  expect(session.health().runtime_faults.size() == 1 &&
             session.health().runtime_faults.contains("fault-a") &&
             !session.health().runtime_error.has_value() &&
             fault_lines ==
                 std::vector<std::string>{
                     "game.json: automation fault: fault-a: faulted during "
                     "event"},
         "a faulting automation must report its canonical current-fault line");

  // A successful event handled by B must not hide A's standing fault.
  const auto triggered = session.trigger("dash", now);
  expect(triggered.triggered == std::vector<std::string>{"dash"} &&
             session.runtime()->snapshot().variables.at("count") ==
                 AutomationValue(std::int64_t{1}) &&
             session.health().runtime_faults.contains("fault-a"),
         "a successful automation must not hide another automation's fault");

  // Retry clears the runtime fault, and the recovered automation then commits.
  session.runtime()->retry("fault-a");
  (void)session.process(started(now));
  expect(session.health().runtime_faults.empty() &&
             !session.health().runtime_error.has_value() &&
             session.health().lines("game.json").empty() &&
             session.runtime()->snapshot().variables.at("ratio") ==
                 AutomationValue(1.0),
         "a retried automation must leave the current health once it commits");

  // A later fault of the same automation is reported again.
  (void)session.process(reset);
  (void)session.process(started(now));
  expect(session.health().runtime_faults.contains("fault-a"),
         "a repeated fault of the same automation must be reported again");

  // Reconfiguration is the second lifecycle owner: a profile that drops A
  // removes its fault instead of keeping a stale identity.
  session.reconfigure_automation(
      accept_automation(health_profile(0.0, false), base));
  expect(session.health().runtime_faults.empty() &&
             session.health().lines("game.json").empty() &&
             !session.automation_rejected(),
         "reconfiguration must retire the faults of removed automations");

  // Stop and start on the same session object cannot resurrect them either.
  session.reconfigure_automation(
      accept_automation(health_profile(0.0, true), base));
  (void)session.process(started(now));
  expect(session.health().runtime_faults.contains("fault-a"),
         "a reconfigured faulting automation must report its current fault");
  (void)session.stop(now);
  expect(session.health().runtime_faults.empty() &&
             session.health().lines("game.json").empty(),
         "stopping a session must drop its runtime faults");
  (void)session.start(accept_automation(health_profile(1.0, true), base), {},
                      now);
  expect(session.health().runtime_faults.empty() &&
             session.health().lines("game.json").empty(),
         "a replacement runtime must not inherit the previous runtime's "
         "faults");
  (void)session.process(reset);
  (void)session.process(started(now));
  expect(session.health().runtime_faults.contains("fault-a"),
         "a new fault of a replacement runtime must be reported");

  // A rejected candidate is reported by the session alone: the running
  // runtime keeps its own current state.
  session.reject_automation(
      {.path = "$.automations[0].rules[0].condition",
       .message = "index expressions require target and key"});
  const auto rejected_lines = session.health().lines("game.json");
  expect(session.automation_rejected() &&
             session.health().rejected.has_value() &&
             rejected_lines.size() == 2 &&
             rejected_lines.front() ==
                 "game.json: automation profile rejected: "
                 "$.automations[0].rules[0].condition: index expressions "
                 "require target and key" &&
             rejected_lines.back() ==
                 "game.json: automation fault: fault-a: faulted during event" &&
             session.runtime() != nullptr,
         "a rejected candidate must be reported beside the running faults");
  session.reconfigure_automation(
      accept_automation(health_profile(1.0, true), base));
  expect(!session.automation_rejected() &&
             !session.health().rejected.has_value() &&
             session.health().lines("game.json").empty(),
         "an accepted reconfiguration must clear the rejection line");
  session.reject_automation({.path = "$.profileId", .message = "mismatch"});
  (void)session.start(accept_automation(health_profile(1.0, true), base), {},
                      now);
  expect(!session.automation_rejected() &&
             !session.health().rejected.has_value(),
         "a new start must clear a recorded rejection");

  // Clearing a rejection drops only the rejected line: the runtime's current
  // faults are unchanged.
  (void)session.process(reset);
  (void)session.process(started(now));
  session.reject_automation({.path = "$.profileId", .message = "mismatch"});
  expect(session.automation_rejected() &&
             session.health().lines("game.json").size() == 2,
         "a recorded rejection must precede the running faults");
  session.clear_automation_rejection();
  expect(!session.automation_rejected() &&
             !session.health().rejected.has_value() &&
             session.health().lines("game.json") ==
                 std::vector<std::string>{
                     "game.json: automation fault: fault-a: faulted during "
                     "event"},
         "clearing a rejection must remove only the rejected line");
}

} // namespace layerc::test
