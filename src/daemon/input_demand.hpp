#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/types.hpp"

#include <array>

namespace layerc::daemon {

// Platform device demand of one session: the union of ordinary binding tokens
// and enabled automation input rules. Each device is requested on its own, so
// no unrelated binding decides whether an automation can observe its input.
struct InputDeviceDemand {
  bool keyboard{};
  bool mouse{};
  bool controller{};
  bool hid{};

  bool operator==(const InputDeviceDemand &) const = default;
};

[[nodiscard]] inline InputDeviceDemand
input_device_demand(const layerc::core::GameProfile &profile,
                    const layerc::core::AutomationProfile *automation) {
  using layerc::core::AutomationEventKind;
  using layerc::core::InputDevice;
  InputDeviceDemand demand;
  const auto request = [&](const InputDevice device) {
    switch (device) {
    case InputDevice::keyboard:
      demand.keyboard = true;
      break;
    case InputDevice::mouse:
      demand.mouse = true;
      break;
    case InputDevice::controller:
      demand.controller = true;
      break;
    case InputDevice::generic_hid:
      demand.hid = true;
      break;
    }
  };
  for (const auto &binding : profile.bindings)
    for (const auto &token : binding.chord.tokens)
      request(token.device);
  if (automation == nullptr)
    return demand;

  for (const auto &entry : automation->automations) {
    if (!entry.enabled)
      continue;
    for (const auto &rule : entry.rules) {
      const bool press = rule.event == AutomationEventKind::input_pressed ||
                         rule.event == AutomationEventKind::input_released;
      // Value changes come from controller axes and HID controls only.
      if (!press && rule.event != AutomationEventKind::input_value_changed)
        continue;
      constexpr std::array press_devices{
          InputDevice::keyboard, InputDevice::mouse, InputDevice::controller};
      for (const auto device : press_devices) {
        if (!press && device != InputDevice::controller)
          continue;
        if (rule.source_filter.empty() ||
            layerc::core::source_filter_names_device(rule.source_filter, device))
          request(device);
      }
      // Generic HID sources always name a configured HID binding.
      if (!profile.hid_bindings.empty() &&
          layerc::core::source_filter_names_device(rule.source_filter,
                                                   InputDevice::generic_hid))
        request(InputDevice::generic_hid);
    }
  }
  return demand;
}

} // namespace layerc::daemon
