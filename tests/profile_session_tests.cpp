#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/profile_session.hpp"

#include <chrono>
#include <cstdint>
#include <string>
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

InputMatcher dash_matcher(const InputToken key) {
  return InputMatcher(std::vector<InputBinding>{
      {.cooldown_id = "dash", .chord = InputChord{{key}}}});
}
} // namespace

// Windows and Linux daemon sessions both derive from core::AutomationSession,
// so this fixes the trigger policy for every platform in one place.
void test_automation_session_trigger_policy() {
  const auto base = session_base();
  const InputToken key{InputDevice::keyboard, 69, 0};
  const auto now = TimePoint{} + 1s;

  AutomationSession valid(base);
  auto valid_matcher = dash_matcher(key);
  const auto fed = valid.feed_input(valid_matcher, InputEvent{key, true}, base, now);
  expect(!valid.blocks_all_triggers() && fed.matched && fed.changed &&
             fed.triggered == std::vector<std::string>{"dash"},
         "a valid session should trigger bound direct input");

  AutomationSession blocked(base);
  blocked.set_invalid_visual_sidecar(true);
  auto blocked_matcher = dash_matcher(key);
  const auto gated =
      blocked.feed_input(blocked_matcher, InputEvent{key, true}, base, now);
  expect(blocked.blocks_all_triggers() && gated.matched && !gated.changed &&
             gated.triggered.empty(),
         "an invalid visual sidecar should match input without triggering");
  expect(blocked.trigger("dash", base, now).triggered.empty(),
         "an invalid visual sidecar should block explicit triggers");
  blocked.cooldowns.snapshot(now, blocked.snapshot_buffer);
  expect(blocked.snapshot_buffer.size() == 1 &&
             blocked.snapshot_buffer.front().available_charges == 1,
         "blocked triggers must not consume cooldown charges");

  blocked.set_invalid_visual_sidecar(false);
  expect(blocked.trigger("dash", base, now).triggered ==
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
  (void)left.start(counting_profile(), {}, base, now);
  (void)right.start(counting_profile(), {}, base, now);
  expect(!left.sync_persistent_to(right),
         "an unchanged session should not sync persistent values");

  const auto triggered = left.trigger("dash", base, now);
  expect(triggered.triggered == std::vector<std::string>{"dash"} &&
             triggered.changed,
         "a triggered cooldown should run cooldown_triggered automations");
  expect(left.sync_persistent_to(right),
         "a dirty session should sync persistent values to a sibling");
  const auto values = right.automation_runtime->snapshot().persistent_values;
  const auto count = values.find("count");
  expect(count != values.end() &&
             count->second == AutomationValue(std::int64_t{1}),
         "the sibling should receive the winning persistent value");
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
