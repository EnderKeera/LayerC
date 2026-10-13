#include "keysym_vk.hpp"

#include <linux/input-event-codes.h>

namespace layerc::platform {
namespace {
struct KeysymVk {
  std::uint32_t keysym;
  std::uint16_t vk;
};
// Numeric equivalents of src/platform/linux/x11_input.cpp's XK_* table.
constexpr KeysymVk special_keys[] = {
    {0xFF1B, 0x1B}, {0xFF09, 0x09}, {0xFF89, 0x09}, // Escape, Tab, KP_Tab
    {0xFF0D, 0x0D}, {0xFF8D, 0x0D}, {0x0020, 0x20}, // Return, KP_Enter, space
    {0xFF80, 0x20}, {0xFF08, 0x08}, {0xFFFF, 0x2E},
    {0xFF9F, 0x2E}, {0xFF63, 0x2D}, {0xFF9E, 0x2D},
    {0xFF50, 0x24}, {0xFF95, 0x24}, {0xFF57, 0x23},
    {0xFF9C, 0x23}, {0xFF55, 0x21}, {0xFF9A, 0x21},
    {0xFF56, 0x22}, {0xFF9B, 0x22}, {0xFF51, 0x25},
    {0xFF96, 0x25}, {0xFF52, 0x26}, {0xFF97, 0x26},
    {0xFF53, 0x27}, {0xFF98, 0x27}, {0xFF54, 0x28},
    {0xFF99, 0x28}, {0xFF9D, 0x0C}, {0xFF61, 0x2C},
    {0xFF14, 0x91}, {0xFF13, 0x13}, {0xFF6B, 0x13},
    {0xFF7F, 0x90}, {0xFFE5, 0x14}, {0xFFE1, 0x10},
    {0xFFE2, 0x10}, {0xFFE3, 0x11}, {0xFFE4, 0x11},
    {0xFFE9, 0x12}, {0xFFEA, 0x12}, {0xFFEB, 0x5B},
    {0xFFEC, 0x5C}, {0xFF67, 0x5D}, {0xFF6A, 0x2F},
    {0xFF69, 0x03}, {0xFFAA, 0x6A}, {0xFFAB, 0x6B},
    {0xFFAC, 0x6C}, {0xFFAD, 0x6D}, {0xFFAE, 0x6E},
    {0xFFAF, 0x6F}, {0xFFB0, 0x60}, {0xFFB1, 0x61},
    {0xFFB2, 0x62}, {0xFFB3, 0x63}, {0xFFB4, 0x64},
    {0xFFB5, 0x65}, {0xFFB6, 0x66}, {0xFFB7, 0x67},
    {0xFFB8, 0x68}, {0xFFB9, 0x69},
    {0x003B, 0xBA}, {0x003D, 0xBB}, {0x002C, 0xBC},
    {0x002D, 0xBD}, {0x002E, 0xBE}, {0x002F, 0xBF},
    {0x0060, 0xC0}, {0x005B, 0xDB}, {0x005C, 0xDC},
    {0x005D, 0xDD}, {0x0027, 0xDE},
};
} // namespace

std::optional<std::uint16_t> keysym_to_vk(const std::uint32_t keysym) noexcept {
  if (keysym >= 0x61 && keysym <= 0x7A)
    return static_cast<std::uint16_t>(keysym - 0x20U);
  if (keysym >= 0x41 && keysym <= 0x5A) return static_cast<std::uint16_t>(keysym);
  if (keysym >= 0x30 && keysym <= 0x39) return static_cast<std::uint16_t>(keysym);
  if (keysym >= 0xFFBE && keysym <= 0xFFD5)
    return static_cast<std::uint16_t>(0x70U + (keysym - 0xFFBE));
  for (const auto &entry : special_keys)
    if (entry.keysym == keysym) return entry.vk;
  return std::nullopt;
}

std::optional<std::uint16_t>
evdev_button_to_mouse_code(const std::uint16_t button) noexcept {
  switch (button) {
  case BTN_LEFT: return 1;
  case BTN_MIDDLE: return 2;
  case BTN_RIGHT: return 3;
  case BTN_SIDE:
  case BTN_BACK: return 8;
  case BTN_EXTRA:
  case BTN_FORWARD: return 9;
  default: return std::nullopt;
  }
}

std::optional<std::uint16_t>
evdev_button_to_controller_code(const std::uint16_t button) noexcept {
  switch (button) {
  case BTN_DPAD_UP: return 1;
  case BTN_DPAD_DOWN: return 2;
  case BTN_DPAD_LEFT: return 3;
  case BTN_DPAD_RIGHT: return 4;
  case BTN_START: return 5;
  case BTN_SELECT: return 6;
  case BTN_THUMBL: return 7;
  case BTN_THUMBR: return 8;
  case BTN_TL: return 9;
  case BTN_TR: return 10;
  case BTN_MODE: return 11;
  case BTN_SOUTH: return 13;
  case BTN_EAST: return 14;
  case BTN_WEST: return 15;
  case BTN_NORTH: return 16;
  case BTN_TL2: return 17;
  case BTN_TR2: return 18;
  default: return std::nullopt;
  }
}
} // namespace layerc::platform
