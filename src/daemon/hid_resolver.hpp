#pragma once

#include "layerc/core/hid.hpp"
#include "layerc/platform/hid_device.hpp"

#include <map>
#include <span>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

class HidResolver {
public:
  using DeviceMap =
      std::map<HANDLE, layerc::platform::HidRawDevice, std::less<>>;

  [[nodiscard]] bool resolves_to(
      const layerc::platform::HidRawDevice &device,
      const layerc::core::HidDeviceSelector &selector,
      const DeviceMap &devices);
  void retain(std::span<const layerc::core::HidDeviceSelector> selectors);
  void clear() noexcept { resolutions_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return resolutions_.size(); }

private:
  struct Resolution {
    layerc::core::HidDeviceSelector selector;
    HANDLE handle{};
  };
  std::vector<Resolution> resolutions_;
};

} // namespace layerc::daemon
