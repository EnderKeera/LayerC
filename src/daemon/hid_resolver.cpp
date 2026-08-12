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
  if (!selector.serial.empty()) {
    const auto matches = std::ranges::count_if(devices, [&](const auto &entry) {
      return basic_match(entry.second, selector) &&
             entry.second.serial == selector.serial;
    });
    if (matches == 1U) {
      const auto found = std::ranges::find_if(devices, [&](const auto &entry) {
        return basic_match(entry.second, selector) &&
               entry.second.serial == selector.serial;
      });
      if (found != devices.end()) resolved = found->first;
    }
  } else if (const auto mapped =
                 layerc::platform::read_hid_stable_slot(selector)) {
    const auto present = std::ranges::find_if(devices, [&](const auto &entry) {
      return basic_match(entry.second, selector) &&
             _wcsicmp(entry.second.path.c_str(), mapped->c_str()) == 0;
    });
    // Never rebind an absent assigned twin to another identical device.
    if (present != devices.end()) resolved = present->first;
  } else {
    std::vector<const layerc::platform::HidRawDevice *> candidates;
    for (const auto &[unused, candidate] : devices) {
      (void)unused;
      if (basic_match(candidate, selector)) candidates.push_back(&candidate);
    }
    if (candidates.size() == 1U) {
      (void)layerc::platform::assign_hid_stable_slot(selector,
                                                     candidates.front()->path);
      resolved = candidates.front()->handle;
    }
  }
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
