#include "layerc/platform/hid_device.hpp"
#include "layerc/platform/settings_store.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <limits>

#include <hidsdi.h>

namespace layerc::platform {
namespace {

std::wstring stable_slot_value_name(
    const layerc::core::HidDeviceSelector &selector) {
  return utf8_to_wide(selector.descriptor_hash) + L"-" +
         std::to_wstring(selector.vendor_id) + L"-" +
         std::to_wstring(selector.product_id) + L"-" +
         std::to_wstring(selector.usage_page) + L"-" +
         std::to_wstring(selector.usage) + L"-slot-" +
         std::to_wstring(selector.stable_slot);
}

std::wstring hid_string(const std::wstring &path,
                        BOOLEAN(__stdcall *reader)(HANDLE, PVOID, ULONG)) {
  const HANDLE device =
      CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (device == INVALID_HANDLE_VALUE) return {};
  std::array<wchar_t, 256> value{};
  const bool available =
      reader(device, value.data(), static_cast<ULONG>(sizeof(value))) != FALSE;
  CloseHandle(device);
  return available ? std::wstring(value.data()) : std::wstring{};
}

std::uint16_t path_hex_component(const std::wstring_view path,
                                 const std::wstring_view marker) {
  const auto offset = path.find(marker);
  if (offset == std::wstring_view::npos || offset + marker.size() + 4U > path.size())
    return 0;
  unsigned value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    const wchar_t character = path[offset + marker.size() + index];
    value <<= 4U;
    if (character >= L'0' && character <= L'9')
      value += static_cast<unsigned>(character - L'0');
    else if (character >= L'a' && character <= L'f')
      value += static_cast<unsigned>(character - L'a' + 10);
    else if (character >= L'A' && character <= L'F')
      value += static_cast<unsigned>(character - L'A' + 10);
    else
      return 0;
  }
  return static_cast<std::uint16_t>(value);
}

std::optional<std::string> system_descriptor_hash(const RID_DEVICE_INFO &info) {
  std::string description = "raw-input:" + std::to_string(info.dwType);
  if (info.dwType == RIM_TYPEKEYBOARD) {
    description += ":" + std::to_string(info.keyboard.dwType) + ":" +
                   std::to_string(info.keyboard.dwSubType) + ":" +
                   std::to_string(info.keyboard.dwKeyboardMode) + ":" +
                   std::to_string(info.keyboard.dwNumberOfFunctionKeys) + ":" +
                   std::to_string(info.keyboard.dwNumberOfIndicators) + ":" +
                   std::to_string(info.keyboard.dwNumberOfKeysTotal);
  } else if (info.dwType == RIM_TYPEMOUSE) {
    description += ":" + std::to_string(info.mouse.dwId) + ":" +
                   std::to_string(info.mouse.dwNumberOfButtons) + ":" +
                   std::to_string(info.mouse.dwSampleRate) + ":" +
                   std::to_string(info.mouse.fHasHorizontalWheel);
  }
  return layerc::core::sha256(std::span(
      reinterpret_cast<const std::byte *>(description.data()),
      description.size()));
}

// HidP_* take the preparsed blob through a non-const pointer but only read it.
PHIDP_PREPARSED_DATA preparsed_data(const std::vector<std::byte> &preparsed) {
  return reinterpret_cast<PHIDP_PREPARSED_DATA>(
      const_cast<std::byte *>(preparsed.data()));
}

layerc::core::HidControlSample unavailable_sample() {
  layerc::core::HidControlSample sample;
  sample.available = false;
  return sample;
}

bool uses_report_ids(const std::vector<std::byte> &preparsed,
                     const HIDP_CAPS &caps) {
  auto *data = preparsed_data(preparsed);
  USHORT button_count = caps.NumberInputButtonCaps;
  std::vector<HIDP_BUTTON_CAPS> buttons(button_count);
  if (button_count != 0 &&
      HidP_GetButtonCaps(HidP_Input, buttons.data(), &button_count, data) ==
          HIDP_STATUS_SUCCESS) {
    buttons.resize(button_count);
    if (std::ranges::any_of(buttons,
                            [](const HIDP_BUTTON_CAPS &value) {
                              return value.ReportID != 0;
                            }))
      return true;
  }
  USHORT value_count = caps.NumberInputValueCaps;
  std::vector<HIDP_VALUE_CAPS> values(value_count);
  if (value_count != 0 &&
      HidP_GetValueCaps(HidP_Input, values.data(), &value_count, data) ==
          HIDP_STATUS_SUCCESS) {
    values.resize(value_count);
    return std::ranges::any_of(values, [](const HIDP_VALUE_CAPS &value) {
      return value.ReportID != 0;
    });
  }
  return false;
}

std::optional<HIDP_VALUE_CAPS>
find_value_caps(const HidRawDevice &device,
                const layerc::core::HidControlBinding &binding) {
  USHORT count = device.caps.NumberInputValueCaps;
  std::vector<HIDP_VALUE_CAPS> values(count);
  if (count == 0 ||
      HidP_GetValueCaps(HidP_Input, values.data(), &count,
                        preparsed_data(device.preparsed)) !=
          HIDP_STATUS_SUCCESS)
    return std::nullopt;
  values.resize(count);
  const auto found = std::ranges::find_if(
      values, [&](const HIDP_VALUE_CAPS &caps) {
        const bool usage_matches =
            caps.IsRange
                ? binding.usage >= caps.Range.UsageMin &&
                      binding.usage <= caps.Range.UsageMax
                : binding.usage == caps.NotRange.Usage;
        const auto requested_report_id =
            device.uses_report_ids ? binding.report_id : 0U;
        return caps.ReportID == requested_report_id &&
               caps.UsagePage == binding.usage_page &&
               caps.LinkCollection == binding.link_collection &&
               usage_matches;
      });
  return found == values.end() ? std::nullopt : std::optional(*found);
}

} // namespace

std::optional<HidRawDevice> load_raw_hid_device(const HANDLE handle) {
  RID_DEVICE_INFO info{};
  info.cbSize = sizeof(info);
  UINT info_size = sizeof(info);
  if (GetRawInputDeviceInfoW(handle, RIDI_DEVICEINFO, &info, &info_size) ==
          static_cast<UINT>(-1) ||
      (info.dwType != RIM_TYPEHID && info.dwType != RIM_TYPEKEYBOARD &&
       info.dwType != RIM_TYPEMOUSE))
    return std::nullopt;
  UINT name_size{};
  if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &name_size) ==
          static_cast<UINT>(-1) ||
      name_size == 0)
    return std::nullopt;
  std::wstring path(name_size, L'\0');
  if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, path.data(),
                             &name_size) == static_cast<UINT>(-1))
    return std::nullopt;
  path.resize(std::wcslen(path.c_str()));
  if (info.dwType != RIM_TYPEHID) {
    const auto hash = system_descriptor_hash(info);
    if (!hash.has_value()) return std::nullopt;
    RID_DEVICE_INFO_HID identity{};
    identity.dwVendorId = path_hex_component(path, L"VID_");
    identity.dwProductId = path_hex_component(path, L"PID_");
    identity.usUsagePage = 0x01U;
    identity.usUsage =
        info.dwType == RIM_TYPEKEYBOARD ? 0x06U : 0x02U;
    std::wstring product = info.dwType == RIM_TYPEKEYBOARD
                               ? L"Device-specific system keyboard"
                               : L"Device-specific system mouse";
    if (identity.dwVendorId != 0 || identity.dwProductId != 0) {
      wchar_t suffix[32]{};
      swprintf_s(suffix, L" %04lX:%04lX", identity.dwVendorId,
                 identity.dwProductId);
      product += suffix;
    }
    return HidRawDevice{.handle = handle,
                        .raw_type = info.dwType,
                        .info = identity,
                        .path = std::move(path),
                        .product_name = std::move(product),
                        .descriptor_hash = *hash,
                        .serial = {},
                        .preparsed = {},
                        .caps = {},
                        .uses_report_ids = false};
  }
  UINT preparsed_size{};
  if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr,
                             &preparsed_size) != 0 ||
      preparsed_size == 0 || preparsed_size > 1024U * 1024U)
    return std::nullopt;
  std::vector<std::byte> preparsed(preparsed_size);
  if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, preparsed.data(),
                             &preparsed_size) == static_cast<UINT>(-1))
    return std::nullopt;
  preparsed.resize(preparsed_size);
  HIDP_CAPS caps{};
  if (HidP_GetCaps(
          reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data()), &caps) !=
      HIDP_STATUS_SUCCESS)
    return std::nullopt;
  const auto hash = layerc::core::sha256(preparsed);
  auto product = hid_string(path, &HidD_GetProductString);
  if (product.empty()) {
    wchar_t fallback[96]{};
    swprintf_s(fallback, L"HID %04lX:%04lX (usage %04X:%04X)",
               info.hid.dwVendorId, info.hid.dwProductId,
               info.hid.usUsagePage, info.hid.usUsage);
    product = fallback;
  }
  const auto serial =
      wide_to_utf8(hid_string(path, &HidD_GetSerialNumberString));
  const bool has_report_ids = uses_report_ids(preparsed, caps);
  return HidRawDevice{
      .handle = handle,
      .raw_type = info.dwType,
      .info = info.hid,
      .path = std::move(path),
      .product_name = std::move(product),
      .descriptor_hash = hash,
      .serial = serial,
      .preparsed = std::move(preparsed),
      .caps = caps,
      .uses_report_ids = has_report_ids};
}

std::vector<HidRawDevice> enumerate_raw_hid_devices() {
  UINT count{};
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 ||
      count == 0)
    return {};
  std::vector<RAWINPUTDEVICELIST> raw(count);
  const UINT loaded =
      GetRawInputDeviceList(raw.data(), &count, sizeof(RAWINPUTDEVICELIST));
  if (loaded == static_cast<UINT>(-1)) return {};
  std::vector<HidRawDevice> output;
  for (std::size_t index = 0; index < loaded; ++index) {
    auto device = load_raw_hid_device(raw[index].hDevice);
    if (device) output.push_back(std::move(*device));
  }
  return output;
}

std::uint8_t effective_hid_report_id(
    const HidRawDevice &device,
    const std::span<const std::byte> report) noexcept {
  if (!device.uses_report_ids || report.empty()) return 0;
  return std::to_integer<std::uint8_t>(report.front());
}

layerc::core::HidControlSample sample_hid_control(
    const HidRawDevice &device,
    const layerc::core::HidControlBinding &binding,
    const std::span<const std::byte> report) {
  if (report.empty() ||
      (device.uses_report_ids && binding.report_id != 0 &&
       effective_hid_report_id(device, report) != binding.report_id))
    return unavailable_sample();
  auto *preparsed = preparsed_data(device.preparsed);
  auto *report_data =
      reinterpret_cast<PCHAR>(const_cast<std::byte *>(report.data()));
  const auto report_size = static_cast<ULONG>(report.size());
  if (binding.activation == layerc::core::HidActivationMode::button) {
    ULONG count = HidP_MaxUsageListLength(HidP_Input, binding.usage_page,
                                          preparsed);
    std::vector<USAGE> usages(count);
    if (count == 0 ||
        HidP_GetUsages(HidP_Input, binding.usage_page,
                       binding.link_collection, usages.data(), &count,
                       preparsed, report_data, report_size) !=
            HIDP_STATUS_SUCCESS)
      return unavailable_sample();
    usages.resize(count);
    layerc::core::HidControlSample sample;
    sample.button_active =
        std::ranges::find(usages, binding.usage) != usages.end();
    return sample;
  }
  ULONG raw{};
  if (HidP_GetUsageValue(HidP_Input, binding.usage_page,
                         binding.link_collection, binding.usage, &raw,
                         preparsed, report_data, report_size) !=
      HIDP_STATUS_SUCCESS)
    return unavailable_sample();
  const auto caps = find_value_caps(device, binding);
  if (!caps) return unavailable_sample();
  layerc::core::HidControlSample sample;
  sample.value = detail::signed_hid_value(raw, *caps);
  sample.logical_min = caps->LogicalMin;
  sample.logical_max = caps->LogicalMax;
  sample.relative = caps->IsAbsolute == FALSE;
  return sample;
}

std::optional<layerc::core::HidControlSample> sample_hid_control(
    const HidRawDevice &device,
    const layerc::core::HidControlBinding &binding,
    const RAWINPUT &input) {
  if (device.raw_type == RIM_TYPEHID || input.header.dwType != device.raw_type ||
      input.header.hDevice != device.handle)
    return std::nullopt;
  layerc::core::HidControlSample sample;
  if (device.raw_type == RIM_TYPEKEYBOARD) {
    if (binding.activation != layerc::core::HidActivationMode::button ||
        binding.usage_page != 0x07U)
      return std::nullopt;
    std::uint16_t code = input.data.keyboard.MakeCode;
    if ((input.data.keyboard.Flags & RI_KEY_E0) != 0)
      code = static_cast<std::uint16_t>(code | 0xE000U);
    if (code != binding.usage) return std::nullopt;
    sample.button_active = (input.data.keyboard.Flags & RI_KEY_BREAK) == 0;
    return sample;
  }

  const auto &mouse = input.data.mouse;
  if (binding.activation == layerc::core::HidActivationMode::button &&
      binding.usage_page == 0x09U && binding.usage >= 1U &&
      binding.usage <= 5U) {
    constexpr USHORT downs[] = {RI_MOUSE_LEFT_BUTTON_DOWN,
                                RI_MOUSE_RIGHT_BUTTON_DOWN,
                                RI_MOUSE_MIDDLE_BUTTON_DOWN,
                                RI_MOUSE_BUTTON_4_DOWN,
                                RI_MOUSE_BUTTON_5_DOWN};
    constexpr USHORT ups[] = {RI_MOUSE_LEFT_BUTTON_UP,
                              RI_MOUSE_RIGHT_BUTTON_UP,
                              RI_MOUSE_MIDDLE_BUTTON_UP,
                              RI_MOUSE_BUTTON_4_UP,
                              RI_MOUSE_BUTTON_5_UP};
    const auto index = static_cast<std::size_t>(binding.usage - 1U);
    if ((mouse.usButtonFlags & (downs[index] | ups[index])) == 0)
      return std::nullopt;
    sample.button_active = (mouse.usButtonFlags & downs[index]) != 0;
    return sample;
  }

  LONG value{};
  bool relative = true;
  if (binding.usage_page == 0x01U && binding.usage == 0x30U) {
    value = mouse.lLastX;
    relative = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
  } else if (binding.usage_page == 0x01U && binding.usage == 0x31U) {
    value = mouse.lLastY;
    relative = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
  } else if (binding.usage_page == 0x01U && binding.usage == 0x38U &&
             (mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
    value = static_cast<SHORT>(mouse.usButtonData);
  } else if (binding.usage_page == 0x0CU && binding.usage == 0x0238U &&
             (mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
    value = static_cast<SHORT>(mouse.usButtonData);
  } else {
    return std::nullopt;
  }
  if (value == 0) return std::nullopt;
  sample.relative = relative;
  sample.value = value;
  sample.logical_min = relative ? std::numeric_limits<SHORT>::min() : 0;
  sample.logical_max = relative ? std::numeric_limits<SHORT>::max() : 65'535;
  return sample;
}

std::optional<std::wstring>
read_hid_stable_slot(const layerc::core::HidDeviceSelector &selector) {
  const auto name = stable_slot_value_name(selector);
  auto value = read_layerc_registry_string(name, 32U * 1024U, L"HidSlots");
  if (!value.value || value.value->empty())
    return std::nullopt;
  return std::move(*value.value);
}

bool assign_hid_stable_slot(
    const layerc::core::HidDeviceSelector &selector,
    const std::wstring_view device_path) {
  if (device_path.empty()) return false;
  const auto name = stable_slot_value_name(selector);
  return static_cast<bool>(
      write_layerc_registry_string(name, device_path, L"HidSlots"));
}

} // namespace layerc::platform
