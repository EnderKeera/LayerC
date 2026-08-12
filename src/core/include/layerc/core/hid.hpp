#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>

namespace layerc::core {

struct HidControlSample {
  bool available{true};
  bool button_active{};
  bool relative{};
  std::int32_t value{};
  std::int32_t logical_min{};
  std::int32_t logical_max{1};
};

struct HidControlState {
  bool initialized{};
  bool pressed{};
};

// Converts HID report values to the Boolean edge contract used by V2.2.
// Unavailable or malformed samples clear the baseline and emit no edge.
[[nodiscard]] std::optional<bool>
evaluate_hid_control(const HidControlBinding &binding,
                     const HidControlSample &sample,
                     HidControlState &state) noexcept;

[[nodiscard]] bool is_system_keyboard_or_mouse(
    const HidDeviceSelector &selector) noexcept;

// Drops unreferenced persisted controls and rewrites compact generic-HID token
// indices. This keeps editor replacements from leaving unreachable descriptors.
void compact_hid_bindings(GameProfile &profile);

} // namespace layerc::core
