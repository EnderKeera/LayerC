#pragma once

#include <cstdint>
#include <optional>

namespace layerc::platform {

// Numeric XKB/X11 keysyms -> profile Win32 VKs. Exact copy of the XI2 table:
// ASCII letters (uppercased), digits, F1-F24, navigation/keypad, modifiers,
// locks, and unshifted OEM punctuation. No X11 headers or connection required.
[[nodiscard]] std::optional<std::uint16_t>
keysym_to_vk(std::uint32_t keysym) noexcept;

// Exact current XI2 button_to_mouse_code results, NOT a new mouse numbering:
// LEFT -> X1 -> 1; MIDDLE -> X2 -> 2; RIGHT -> X3 -> 3;
// SIDE/BACK -> X8 -> 8; EXTRA/FORWARD -> X9 -> 9.
// REL_WHEEL positive -> X4 -> 6; negative -> X5 -> 7 (handled by input source).
// Note: current profile validation only accepts mouse codes 1..7. The
// integrator must reconcile XI2's 8/9 with portable side-button bindings.
[[nodiscard]] std::optional<std::uint16_t>
evdev_button_to_mouse_code(std::uint16_t button) noexcept;

// Windows poll_controllers publishes XInput wButtons bit index + 1, then
// left/right triggers as 17/18. Linux maps positions, not printed ABXY labels:
// SOUTH(A)=13, EAST(B)=14, WEST(X)=15, NORTH(Y)=16, TL=9, TR=10,
// TL2=17, TR2=18, SELECT(Back)=6, START=5, THUMBL=7, THUMBR=8,
// DPAD_UP=1, DOWN=2, LEFT=3, RIGHT=4. MODE(Guide)=11 is the reserved
// XInput bit 10; like Windows' bit loop it can be emitted, but current profile
// validation rejects 11/12. Unknown buttons have no mapping.
[[nodiscard]] std::optional<std::uint16_t>
evdev_button_to_controller_code(std::uint16_t button) noexcept;

} // namespace layerc::platform
