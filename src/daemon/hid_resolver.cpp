#include "hid_resolver.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace {

bool basic_match(const layerc::platform::HidRawDevice &device,
                 const layerc::core::HidDeviceSelector &selector) {
  return device.info.dwVendorId == selector.vendor_id &&
         device.info.dwProductId == selector.product_id &&
         device.info.dwVersionNumber == selector.version &&
         device.info.usUsagePage == selector.usage_page &&
         device.info.usUsage == selector.usage &&
         device.descriptor_hash == selector.descriptor_hash;
}

// Serial match wins when exactly one device carries the selector's serial.
HANDLE resolve_by_serial(const HidResolver::DeviceMap &devices,
                         const layerc::core::HidDeviceSelector &selector) {
  const auto serial_match = [&](const auto &entry) {
    return basic_match(entry.second, selector) &&
           entry.second.serial == selector.serial;
  };
  if (std::ranges::count_if(devices, serial_match) != 1U)
    return {};
  const auto found = std::ranges::find_if(devices, serial_match);
  return found == devices.end() ? HANDLE{} : found->first;
}

// A previously assigned stable slot is honored only while that exact device
// is present: never rebind an absent assigned twin to another identical device.
HANDLE resolve_by_stable_slot(const HidResolver::DeviceMap &devices,
                              const layerc::core::HidDeviceSelector &selector,
                              const std::wstring &mapped_path) {
  const auto present = std::ranges::find_if(devices, [&](const auto &entry) {
    return basic_match(entry.second, selector) &&
           _wcsicmp(entry.second.path.c_str(), mapped_path.c_str()) == 0;
  });
  return present == devices.end() ? HANDLE{} : present->first;
}

// Unassigned selectors bind (and persist a stable slot) only when exactly one
// matching device is connected.
HANDLE resolve_unique_candidate(
    const HidResolver::DeviceMap &devices,
    const layerc::core::HidDeviceSelector &selector) {
  std::vector<const layerc::platform::HidRawDevice *> candidates;
  for (const auto &[unused, candidate] : devices) {
    (void)unused;
    if (basic_match(candidate, selector)) candidates.push_back(&candidate);
  }
  if (candidates.size() != 1U)
    return {};
  (void)layerc::platform::assign_hid_stable_slot(selector,
                                                 candidates.front()->path);
  return candidates.front()->handle;
}

} // namespace

bool HidResolver::resolves_to(
    const layerc::platform::HidRawDevice &device,
    const layerc::core::HidDeviceSelector &selector,
    const DeviceMap &devices) {
  if (!basic_match(device, selector)) return false;
  const auto cached =
      std::ranges::find(resolutions_, selector, &Resolution::selector);
  if (cached != resolutions_.end()) return cached->handle == device.handle;

  HANDLE resolved{};
  if (!selector.serial.empty())
    resolved = resolve_by_serial(devices, selector);
  else if (const auto mapped =
               layerc::platform::read_hid_stable_slot(selector))
    resolved = resolve_by_stable_slot(devices, selector, *mapped);
  else
    resolved = resolve_unique_candidate(devices, selector);
  resolutions_.push_back({.selector = selector, .handle = resolved});
  return resolved == device.handle;
}

void HidResolver::retain(
    const std::span<const layerc::core::HidDeviceSelector> selectors) {
  std::erase_if(resolutions_, [&](const auto &resolution) {
    return std::ranges::find(selectors, resolution.selector) == selectors.end();
  });
}

} // namespace layerc::daemon
