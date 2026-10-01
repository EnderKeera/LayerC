#include "daemon_runtime_services.hpp"
#include "input_demand.hpp"
#include "maintenance_schedule.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

using layerc::daemon::every_capability;
using layerc::daemon::OwnedCapabilityServices;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileResult;
using layerc::test::expect;

constexpr std::size_t slot(const Capability capability) noexcept {
  return static_cast<std::size_t>(capability);
}

// Records every typed dispatch so the adapters can be checked for routing.
struct FakeOwner {
  std::array<ServiceReconcileResult, layerc::runtime::capability_count> results{};
  std::array<RuntimeResourceStats, layerc::runtime::capability_count> owned{};
  std::array<unsigned, layerc::runtime::capability_count> reconcile_calls{};
  std::array<unsigned, layerc::runtime::capability_count> quiesce_calls{};
  mutable std::array<unsigned, layerc::runtime::capability_count> resource_calls{};
  std::array<std::uint32_t, layerc::runtime::capability_count>
      observed_consumers{};
  mutable Capability last_capability{Capability::count};

  ServiceReconcileResult reconcile_capability(const Capability capability,
                                              const CapabilityDemand &demand) {
    ++reconcile_calls[slot(capability)];
    observed_consumers[slot(capability)] = demand.consumers(capability);
    last_capability = capability;
    return results[slot(capability)];
  }
  void quiesce_capability(const Capability capability) noexcept {
    ++quiesce_calls[slot(capability)];
    last_capability = capability;
  }
  RuntimeResourceStats
  capability_resources(const Capability capability) const noexcept {
    ++resource_calls[slot(capability)];
    last_capability = capability;
    return owned[slot(capability)];
  }
};

void adapters_preserve_capability_identity_and_results() {
  FakeOwner owner;
  OwnedCapabilityServices<FakeOwner> services(owner, every_capability());
  const auto registry = services.registry();

  for (std::size_t index = 0; index < registry.size(); ++index) {
    const auto capability = static_cast<Capability>(index);
    auto *service = registry[index];
    expect(service != nullptr, "every runtime capability should have an adapter");
    if (service == nullptr)
      continue;

    CapabilityDemand demand;
    demand.set(capability, static_cast<std::uint32_t>(index + 1U));
    owner.results[index] =
        index == slot(Capability::visual)
            ? ServiceReconcileResult{false, "capture unavailable"}
            : ServiceReconcileResult{};
    owner.owned[index] = {.handles = static_cast<std::uint32_t>(index + 10U),
                          .objects = static_cast<std::uint32_t>(index + 20U)};

    const auto reconciled = service->reconcile(demand);
    expect(reconciled.success == owner.results[index].success &&
               reconciled.error == owner.results[index].error,
           "adapter should preserve typed reconcile results");
    expect(owner.last_capability == capability &&
               owner.observed_consumers[index] == index + 1U,
           "adapter should route demand to the matching capability");
    expect(service->resources() == owner.owned[index],
           "adapter should report only its capability's owned resources");
    service->quiesce();
    expect(owner.last_capability == capability,
           "adapter should quiesce the matching capability");
  }

  for (std::size_t index = 0; index < registry.size(); ++index) {
    expect(owner.reconcile_calls[index] == 1 &&
               owner.resource_calls[index] == 1 &&
               owner.quiesce_calls[index] == 1,
           "each adapter operation should reach the owner exactly once");
  }
}

void unserved_capability_stays_unregistered() {
  FakeOwner owner;
  const std::array<Capability, 1> served{{Capability::input}};
  OwnedCapabilityServices<FakeOwner> services(owner, served);
  const auto registry = services.registry();
  expect(registry[slot(Capability::input)] != nullptr,
         "a served capability should be registered");
  for (std::size_t index = 1; index < registry.size(); ++index)
    expect(registry[index] == nullptr,
           "an unserved capability should stay unregistered");
}

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

} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(adapters_preserve_capability_identity_and_results),
    LAYERC_TEST_CASE(unserved_capability_stays_unregistered),
    LAYERC_TEST_CASE(busy_wakeups_cannot_postpone_maintenance),
    LAYERC_TEST_CASE(automation_input_rules_request_their_own_devices))
