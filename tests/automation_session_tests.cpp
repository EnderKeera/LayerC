#include "core_test_cases.hpp"
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
  (void)left.start(counting_profile(), {}, now);
  (void)right.start(counting_profile(), {}, now);
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
  (void)session.start(axis_profile(), {}, now);

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

} // namespace layerc::test
