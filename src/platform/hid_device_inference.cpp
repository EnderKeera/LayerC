#include "layerc/platform/hid_device.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace layerc::platform {
namespace {

layerc::core::HidDeviceSelector selector_for(const HidRawDevice &device) {
  return {.vendor_id = static_cast<std::uint16_t>(device.info.dwVendorId),
          .product_id = static_cast<std::uint16_t>(device.info.dwProductId),
          .version = static_cast<std::uint16_t>(device.info.dwVersionNumber),
          .usage_page = device.info.usUsagePage,
          .usage = device.info.usUsage,
          .descriptor_hash = device.descriptor_hash,
          .serial = device.serial,
          .stable_slot = 1};
}

} // namespace

std::optional<InferredHidControl>
infer_hid_control(const HidRawDevice &device,
                  const std::span<const std::byte> report,
                  const std::span<const std::byte> baseline) {
  if (device.raw_type != RIM_TYPEHID || report.empty() || baseline.empty())
    return std::nullopt;
  const auto report_id = effective_hid_report_id(device, report);
  const auto baseline_report_id = effective_hid_report_id(device, baseline);
  if (report_id != baseline_report_id) return std::nullopt;
  auto *preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(
      const_cast<std::byte *>(device.preparsed.data()));
  auto *report_data =
      reinterpret_cast<PCHAR>(const_cast<std::byte *>(report.data()));
  auto *baseline_data =
      reinterpret_cast<PCHAR>(const_cast<std::byte *>(baseline.data()));
  const auto report_size = static_cast<ULONG>(report.size());
  const auto baseline_size = static_cast<ULONG>(baseline.size());
  USHORT button_count = device.caps.NumberInputButtonCaps;
  std::vector<HIDP_BUTTON_CAPS> buttons(button_count);
  if (button_count != 0 &&
      HidP_GetButtonCaps(HidP_Input, buttons.data(), &button_count,
                         preparsed) == HIDP_STATUS_SUCCESS) {
    buttons.resize(button_count);
    for (const auto &caps : buttons) {
      if (caps.ReportID != report_id) continue;
      ULONG usage_count = HidP_MaxUsageListLength(HidP_Input, caps.UsagePage,
                                                  preparsed);
      std::vector<USAGE> usages(usage_count);
      ULONG baseline_count = usage_count;
      std::vector<USAGE> baseline_usages(baseline_count);
      if (usage_count == 0 ||
          HidP_GetUsages(HidP_Input, caps.UsagePage, caps.LinkCollection,
                         usages.data(), &usage_count, preparsed, report_data,
                         report_size) != HIDP_STATUS_SUCCESS ||
          HidP_GetUsages(HidP_Input, caps.UsagePage, caps.LinkCollection,
                         baseline_usages.data(), &baseline_count, preparsed,
                         baseline_data, baseline_size) != HIDP_STATUS_SUCCESS)
        continue;
      usages.resize(usage_count);
      baseline_usages.resize(baseline_count);
      const auto changed = std::ranges::find_if(usages, [&](const USAGE usage) {
        return std::ranges::find(baseline_usages, usage) ==
               baseline_usages.end();
      });
      const auto released = std::ranges::find_if(
          baseline_usages, [&](const USAGE usage) {
            return std::ranges::find(usages, usage) == usages.end();
          });
      if (changed == usages.end() && released == baseline_usages.end())
        continue;
      const USAGE usage =
          changed != usages.end() ? *changed : *released;
      InferredHidControl result;
      result.binding.device = selector_for(device);
      result.binding.report_id = report_id;
      result.binding.usage_page = caps.UsagePage;
      result.binding.usage = usage;
      result.binding.link_collection = caps.LinkCollection;
      result.binding.activation = layerc::core::HidActivationMode::button;
      result.description = L"Button, usage " +
                           std::to_wstring(caps.UsagePage) + L":" +
                           std::to_wstring(usage);
      return result;
    }
  }

  USHORT value_count = device.caps.NumberInputValueCaps;
  std::vector<HIDP_VALUE_CAPS> values(value_count);
  if (value_count == 0 ||
      HidP_GetValueCaps(HidP_Input, values.data(), &value_count, preparsed) !=
          HIDP_STATUS_SUCCESS)
    return std::nullopt;
  values.resize(value_count);
  struct Candidate {
    HIDP_VALUE_CAPS caps{};
    USAGE usage{};
    std::int32_t value{};
    double strength{};
  };
  std::optional<Candidate> best;
  for (const auto &caps : values) {
    if (caps.ReportID != report_id) continue;
    const USAGE minimum = caps.IsRange ? caps.Range.UsageMin
                                       : caps.NotRange.Usage;
    const USAGE maximum = caps.IsRange ? caps.Range.UsageMax
                                       : caps.NotRange.Usage;
    if (maximum < minimum || maximum - minimum > 128) continue;
    for (USAGE usage = minimum; usage <= maximum; ++usage) {
      ULONG raw{};
      ULONG baseline_raw{};
      if (HidP_GetUsageValue(HidP_Input, caps.UsagePage,
                             caps.LinkCollection, usage, &raw, preparsed,
                             report_data, report_size) != HIDP_STATUS_SUCCESS ||
          HidP_GetUsageValue(HidP_Input, caps.UsagePage,
                             caps.LinkCollection, usage, &baseline_raw,
                             preparsed, baseline_data, baseline_size) !=
              HIDP_STATUS_SUCCESS)
        continue;
      const auto value = detail::signed_hid_value(raw, caps);
      const auto idle_value = detail::signed_hid_value(baseline_raw, caps);
      if (value == idle_value) continue;
      const double range =
          std::max(1.0, static_cast<double>(caps.LogicalMax) -
                            static_cast<double>(caps.LogicalMin));
      const double strength =
          std::abs(static_cast<double>(value) - idle_value) / range;
      if (!best || strength > best->strength)
        best = Candidate{caps, usage, value, strength};
      if (usage == std::numeric_limits<USAGE>::max()) break;
    }
  }
  if (!best || best->strength < 0.05) return std::nullopt;
  InferredHidControl result;
  result.binding.device = selector_for(device);
  result.binding.report_id = report_id;
  result.binding.usage_page = best->caps.UsagePage;
  result.binding.usage = best->usage;
  result.binding.link_collection = best->caps.LinkCollection;
  const auto range = static_cast<std::int64_t>(best->caps.LogicalMax) -
                     best->caps.LogicalMin;
  if (best->caps.IsAbsolute == FALSE) {
    result.binding.activation =
        best->value < 0
            ? layerc::core::HidActivationMode::relative_negative
            : layerc::core::HidActivationMode::relative_positive;
    result.description = best->value < 0 ? L"Relative axis negative"
                                         : L"Relative axis positive";
  } else if (range >= 0 && range <= 16) {
    result.binding.activation = layerc::core::HidActivationMode::equals;
    result.binding.discrete_value = best->value;
    result.description = L"Discrete value " + std::to_wstring(best->value);
  } else {
    const double normalized =
        range <= 0 ? 0.5
                   : (static_cast<double>(best->value) -
                      best->caps.LogicalMin) /
                         static_cast<double>(range);
    result.binding.activation =
        normalized < 0.5
            ? layerc::core::HidActivationMode::axis_negative
            : layerc::core::HidActivationMode::axis_positive;
    result.binding.threshold =
        std::clamp(normalized < 0.5 ? 1.0 - normalized : normalized,
                   0.55, 0.95);
    result.description = normalized < 0.5 ? L"Absolute axis negative"
                                          : L"Absolute axis positive";
  }
  result.description += L", usage " +
                        std::to_wstring(result.binding.usage_page) + L":" +
                        std::to_wstring(result.binding.usage);
  return result;
}

std::optional<InferredHidControl>
infer_hid_control(const HidRawDevice &device, const RAWINPUT &input) {
  if (device.raw_type == RIM_TYPEHID || input.header.dwType != device.raw_type ||
      input.header.hDevice != device.handle)
    return std::nullopt;
  InferredHidControl result;
  result.binding.device = selector_for(device);
  if (device.raw_type == RIM_TYPEKEYBOARD) {
    std::uint16_t code = input.data.keyboard.MakeCode;
    if ((input.data.keyboard.Flags & RI_KEY_E0) != 0)
      code = static_cast<std::uint16_t>(code | 0xE000U);
    if (code == 0) return std::nullopt;
    result.binding.usage_page = 0x07U;
    result.binding.usage = code;
    result.binding.activation = layerc::core::HidActivationMode::button;
    result.description = L"Device-specific key, scan " +
                         std::to_wstring(code);
    return result;
  }

  const auto &mouse = input.data.mouse;
  constexpr USHORT button_flags[] = {
      RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP,
      RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP,
      RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP,
      RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP,
      RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP};
  for (std::size_t index = 0; index < std::size(button_flags); ++index) {
    if ((mouse.usButtonFlags & button_flags[index]) == 0) continue;
    result.binding.usage_page = 0x09U;
    result.binding.usage = static_cast<std::uint16_t>(index + 1U);
    result.binding.activation = layerc::core::HidActivationMode::button;
    result.description = L"Device-specific mouse button " +
                         std::to_wstring(index + 1U);
    return result;
  }

  LONG value{};
  if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
    value = static_cast<SHORT>(mouse.usButtonData);
    result.binding.usage_page = 0x01U;
    result.binding.usage = 0x38U;
    result.description = L"Device-specific mouse wheel";
  } else if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
    value = static_cast<SHORT>(mouse.usButtonData);
    result.binding.usage_page = 0x0CU;
    result.binding.usage = 0x0238U;
    result.description = L"Device-specific horizontal wheel";
  } else if (std::abs(static_cast<std::int64_t>(mouse.lLastX)) >=
                 std::abs(static_cast<std::int64_t>(mouse.lLastY)) &&
             mouse.lLastX != 0) {
    value = mouse.lLastX;
    result.binding.usage_page = 0x01U;
    result.binding.usage = 0x30U;
    result.description = L"Device-specific mouse X movement";
  } else if (mouse.lLastY != 0) {
    value = mouse.lLastY;
    result.binding.usage_page = 0x01U;
    result.binding.usage = 0x31U;
    result.description = L"Device-specific mouse Y movement";
  } else {
    return std::nullopt;
  }
  const bool relative = (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
  if (relative) {
    result.binding.activation =
        value < 0 ? layerc::core::HidActivationMode::relative_negative
                  : layerc::core::HidActivationMode::relative_positive;
  } else {
    const double normalized =
        std::clamp(static_cast<double>(value) / 65'535.0, 0.0, 1.0);
    result.binding.activation =
        normalized < 0.5 ? layerc::core::HidActivationMode::axis_negative
                         : layerc::core::HidActivationMode::axis_positive;
    result.binding.threshold =
        std::clamp(normalized < 0.5 ? 1.0 - normalized : normalized,
                   0.55, 0.95);
  }
  return result;
}

} // namespace layerc::platform
