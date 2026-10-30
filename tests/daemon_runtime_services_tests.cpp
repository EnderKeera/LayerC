#include "daemon_owner_tests.hpp"
#include "input_demand.hpp"
#include "maintenance_schedule.hpp"
#include "overlay_cadence.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

using layerc::test::expect;

void busy_wakeups_cannot_postpone_maintenance() {
  using layerc::core::Milliseconds;
  using namespace std::chrono_literals;
  layerc::daemon::MaintenanceSchedule schedule;
  auto now = layerc::core::TimePoint{} + 1s;
  const auto started = now;
  auto last_run = now;
  auto longest_gap = Milliseconds{0};
  int runs{};
  // A stalled helper needs a 500 ms watchdog check while 16 ms timers, input,
  // and another capture's snapshots wake the loop before it can ever time out.
  while (now - started < 3s) {
    const auto wait = schedule.wait_for(now, Milliseconds{500});
    if (!expect(wait.has_value() && *wait <= Milliseconds{500},
                "an armed schedule should bound the blocking wait"))
      return;
    now += std::min(*wait, Milliseconds{16});
    if (schedule.take_due(now)) {
      longest_gap = std::max(
          longest_gap, std::chrono::duration_cast<Milliseconds>(now - last_run));
      last_run = now;
      ++runs;
    }
  }
  expect(runs >= 5 && longest_gap <= Milliseconds{516},
         "maintenance must stay on its deadline under continuous wake-ups");

  expect(schedule.wait_for(now, Milliseconds{250}) <= Milliseconds{250},
         "a shorter interval (live-edit lease) should pull the deadline in");
  expect(!schedule.wait_for(now, std::nullopt) && !schedule.take_due(now + 1h),
         "no periodic maintenance leaves the loop idle-blocking");
}

layerc::core::AutomationProfile
input_automation(const layerc::core::AutomationEventKind event,
                 std::string filter, const bool enabled = true) {
  layerc::core::AutomationProfile profile;
  layerc::core::AutomationDefinition definition;
  definition.id = "rule-owner";
  definition.enabled = enabled;
  layerc::core::AutomationRule rule;
  rule.id = "rule";
  rule.event = event;
  rule.source_filter = std::move(filter);
  definition.rules.push_back(std::move(rule));
  profile.automations.push_back(std::move(definition));
  return profile;
}

void automation_input_rules_request_their_own_devices() {
  using layerc::core::AutomationEventKind;
  using layerc::core::InputDevice;
  using layerc::core::input_source_id;
  using layerc::daemon::InputDeviceDemand;
  using layerc::daemon::input_device_demand;
  const layerc::core::GameProfile no_bindings;
  const auto demand = [&](const layerc::core::AutomationProfile &automation) {
    return input_device_demand(no_bindings, &automation);
  };
  expect(input_device_demand(no_bindings, nullptr) == InputDeviceDemand{},
         "a profile without bindings or automations requests no device");
  const auto keyboard_source = input_source_id({InputDevice::keyboard, 0x1E});
  const auto mouse_source = input_source_id({InputDevice::mouse, 1});
  const auto controller_source =
      input_source_id({InputDevice::controller, 0x1000, 0});
  expect(keyboard_source == "keyboard:30" && mouse_source == "mouse:1" &&
             controller_source == "controller:4096:0",
         "input filters use decimal codes, with the controller index last");
  expect(demand(input_automation(AutomationEventKind::input_pressed,
                                 keyboard_source)) ==
             InputDeviceDemand{.keyboard = true},
         "a keyboard press automation registers the keyboard on its own");
  expect(demand(input_automation(AutomationEventKind::input_released,
                                 mouse_source)) ==
             InputDeviceDemand{.mouse = true},
         "a mouse release automation registers the mouse without any binding");
  expect(demand(input_automation(AutomationEventKind::input_pressed,
                                 controller_source)) ==
             InputDeviceDemand{.controller = true},
         "a controller-button automation enables controller polling");
  expect(demand(input_automation(AutomationEventKind::input_pressed, "")) ==
             InputDeviceDemand{.keyboard = true, .mouse = true, .controller = true},
         "an unfiltered press automation observes every press source");
  expect(demand(input_automation(AutomationEventKind::input_value_changed, "")) ==
             InputDeviceDemand{.controller = true},
         "value changes come from controller axes, not keys or buttons");
  expect(demand(input_automation(AutomationEventKind::input_pressed, "",
                                 /*enabled=*/false)) == InputDeviceDemand{} &&
             demand(input_automation(AutomationEventKind::input_pressed,
                                     layerc::core::hid_source_id(1))) ==
                 InputDeviceDemand{} &&
             demand(input_automation(AutomationEventKind::session_started, "")) ==
                 InputDeviceDemand{},
         "disabled rules, unconfigured HID filters, and non-input events "
         "request no device");
  expect(demand(input_automation(
             AutomationEventKind::input_value_changed,
             layerc::core::controller_axis_source_id(0, "left_x"))) ==
             InputDeviceDemand{.controller = true},
         "a filtered controller axis also enables polling");
  auto bound = no_bindings;
  layerc::core::InputBinding binding;
  binding.chord.tokens.push_back({InputDevice::keyboard, 0x1E});
  bound.bindings.push_back(std::move(binding));
  const auto mouse_rule = input_automation(AutomationEventKind::input_pressed,
                                          mouse_source);
  expect(input_device_demand(bound, &mouse_rule) ==
             InputDeviceDemand{.keyboard = true, .mouse = true},
         "automation device demand composes with ordinary bindings");
  bound.hid_bindings.emplace_back();
  const auto hid_rule = input_automation(AutomationEventKind::input_pressed,
                                        layerc::core::hid_source_id(0));
  expect(input_device_demand(bound, &hid_rule) ==
             InputDeviceDemand{.keyboard = true, .hid = true},
         "a configured HID filter requests its HID device");
}

void typed_overlay_cadence_is_demand_driven() {
  using namespace layerc::core;
  using layerc::daemon::overlay_cadence;
  const TimePoint now{};
  PanelPresentation frame{.visible = true};
  frame.items.push_back({.kind = OverlayItemKind::counter, .has_progress = false});
  expect(!overlay_cadence(std::span(&frame, 1), false, now).delay,
         "a static counter creates no render timer");
  const auto running = overlay_cadence(std::span(&frame, 1), true, now);
  expect(running.continuous && running.delay == Milliseconds{16},
         "an engine cooldown retains the existing 16 ms cadence");
  frame.items.front().kind = OverlayItemKind::timer;
  frame.items.front().active = true;
  expect(overlay_cadence(std::span(&frame, 1), false, now).continuous,
         "an active-duration or automation timer animates continuously");
  frame.items.front().kind = OverlayItemKind::indicator;
  frame.next_wakeup = now + Milliseconds{125};
  const auto flash = overlay_cadence(std::span(&frame, 1), false, now);
  expect(!flash.continuous && flash.delay == Milliseconds{125},
         "a flashing indicator wakes only at its next edge");
  frame.next_wakeup = now + std::chrono::microseconds{1};
  expect(overlay_cadence(std::span(&frame, 1), false, now).delay == Milliseconds{1},
         "fractional millisecond deadlines round up, never busy-spin");
  frame.next_wakeup = now - Milliseconds{1};
  expect(overlay_cadence(std::span(&frame, 1), false, now).delay == Milliseconds{1},
         "a past edge is bounded by a one-millisecond retry");
  frame.visible = false;
  expect(!overlay_cadence(std::span(&frame, 1), false, now).delay,
         "hidden panels do not request wakeups");
  frame.visible = true;
  frame.next_wakeup.reset();
  frame.items.front().kind = OverlayItemKind::cooldown;
  frame.items.front().ready = false;
  frame.items.front().unavailable = true;
  expect(!overlay_cadence(std::span(&frame, 1), false, now).delay,
         "unavailable rows never create a countdown timer");
}

} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(busy_wakeups_cannot_postpone_maintenance),
    LAYERC_TEST_CASE(typed_overlay_cadence_is_demand_driven),
    LAYERC_TEST_CASE(automation_input_rules_request_their_own_devices),
    LAYERC_TEST_CASE(
        layerc::test::daemon_owner::input_owner_dispatches_without_a_daemon_app),
    LAYERC_TEST_CASE(
        layerc::test::daemon_owner::dormant_owners_do_not_recreate_resources),
    LAYERC_TEST_CASE(
        layerc::test::daemon_owner::automation_owner_controls_runtime_and_trace),
    LAYERC_TEST_CASE(
        layerc::test::daemon_owner::live_owner_owns_control_resources_and_faults))
