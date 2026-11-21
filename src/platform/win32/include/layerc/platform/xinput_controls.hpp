#pragma once

#include "layerc/core/types.hpp"

#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace layerc::platform {

// LayerC's digital XInput layout: wButtons in bits 0–15, then the two
// triggers. The daemon reports the full field; capture filters reserved bits.
inline constexpr unsigned xinput_digital_control_count = 18;
// Codes 11 and 12 are reserved in the profile format.
inline constexpr auto xinput_bindable_controls = std::uint32_t{0x3f3ff};

[[nodiscard]] constexpr std::uint32_t
xinput_digital_controls(const XINPUT_GAMEPAD &gamepad) noexcept {
  auto bits = static_cast<std::uint32_t>(gamepad.wButtons);
  if (gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
    bits |= std::uint32_t{1} << 16U;
  if (gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
    bits |= std::uint32_t{1} << 17U;
  return bits;
}

[[nodiscard]] constexpr core::InputToken xinput_control_token(
    const unsigned bit, const DWORD index) noexcept {
  return {core::InputDevice::controller, static_cast<std::uint16_t>(bit + 1U),
          static_cast<std::uint8_t>(index)};
}

} // namespace layerc::platform
