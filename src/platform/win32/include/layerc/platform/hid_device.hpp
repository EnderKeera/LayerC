#pragma once

#include "layerc/core/hid.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

namespace layerc::platform {

struct HidRawDevice {
  HANDLE handle{};
  DWORD raw_type{RIM_TYPEHID};
  RID_DEVICE_INFO_HID info{};
  std::wstring path;
  std::wstring product_name;
  std::string descriptor_hash;
  std::string serial;
  std::vector<std::byte> preparsed;
  HIDP_CAPS caps{};
  bool uses_report_ids{};
};

struct InferredHidControl {
  layerc::core::HidControlBinding binding;
  std::wstring description;
};

namespace detail {
[[nodiscard]] inline std::int32_t
signed_hid_value(ULONG raw, const HIDP_VALUE_CAPS &caps) noexcept {
  if (caps.LogicalMin < 0 && caps.BitSize > 0 && caps.BitSize < 32) {
    const auto sign = 1U << (caps.BitSize - 1U);
    const auto mask = (1U << caps.BitSize) - 1U;
    raw &= mask;
    if ((raw & sign) != 0U)
      raw |= ~mask;
  }
  return static_cast<std::int32_t>(raw);
}
} // namespace detail

[[nodiscard]] std::optional<HidRawDevice>
load_raw_hid_device(HANDLE handle);
[[nodiscard]] std::vector<HidRawDevice> enumerate_raw_hid_devices();
[[nodiscard]] std::uint8_t effective_hid_report_id(
    const HidRawDevice &device, std::span<const std::byte> report) noexcept;
[[nodiscard]] layerc::core::HidControlSample
sample_hid_control(const HidRawDevice &device,
                   const layerc::core::HidControlBinding &binding,
                   std::span<const std::byte> report);
[[nodiscard]] std::optional<InferredHidControl>
infer_hid_control(const HidRawDevice &device,
                  std::span<const std::byte> report,
                  std::span<const std::byte> baseline = {});
[[nodiscard]] std::optional<InferredHidControl>
infer_hid_control(const HidRawDevice &device, const RAWINPUT &input);
[[nodiscard]] std::optional<layerc::core::HidControlSample>
sample_hid_control(const HidRawDevice &device,
                   const layerc::core::HidControlBinding &binding,
                   const RAWINPUT &input);
[[nodiscard]] std::optional<std::wstring>
read_hid_stable_slot(const layerc::core::HidDeviceSelector &selector);
[[nodiscard]] bool
assign_hid_stable_slot(const layerc::core::HidDeviceSelector &selector,
                       std::wstring_view device_path);

} // namespace layerc::platform
