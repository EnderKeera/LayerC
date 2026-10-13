#include "evdev_keymap.hpp"
#include "keysym_vk.hpp"
#include "test_support.hpp"

#ifdef LAYERC_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

#include <cstdlib>
#include <string>
#include <utility>

namespace {
using layerc::test::expect;
using namespace layerc::platform;

void test_keysym_table() {
  for (std::uint32_t code = 'a'; code <= 'z'; ++code)
    expect(keysym_to_vk(code) == code - 0x20U, "lowercase letter -> VK");
  for (std::uint32_t code = 'A'; code <= 'Z'; ++code)
    expect(keysym_to_vk(code) == code, "uppercase letter -> VK");
  for (std::uint32_t code = '0'; code <= '9'; ++code)
    expect(keysym_to_vk(code) == code, "digit -> VK");
  for (std::uint32_t code = 0xFFBE; code <= 0xFFD5; ++code)
    expect(keysym_to_vk(code) == 0x70U + code - 0xFFBE, "F1..F24 -> VK");
  constexpr std::pair<std::uint32_t, std::uint16_t> special[] = {
      {0xFF1B, 0x1B}, {0xFF09, 0x09}, {0xFF89, 0x09},
      {0xFF0D, 0x0D}, {0xFF8D, 0x0D}, {0x0020, 0x20},
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
  for (const auto &[symbol, vk] : special)
    expect(keysym_to_vk(symbol) == vk, "exact XI2 special/OEM mapping");
  expect(!keysym_to_vk(0), "unknown keysym ignored");
  expect(!keysym_to_vk(0x1008FF12), "media key not silently assigned a VK");
}

#ifdef LAYERC_HAVE_XKBCOMMON
std::string keymap_text(const char *layout) {
  auto *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!expect(context != nullptr, "XKB context")) return {};
  xkb_rule_names names{};
  names.layout = layout;
  auto *map = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref(context);
  if (!expect(map != nullptr, "XKB map from explicit layout names")) return {};
  char *text = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
  std::string result = text != nullptr ? text : "";
  std::free(text);
  xkb_keymap_unref(map);
  return result;
}
#endif

void test_keymap_layouts() {
  std::string error;
  const auto defaults = EvdevKeymap::create(std::nullopt, error);
  if (!expect(defaults.has_value() && error.empty(), "default keymap")) return;
  expect(defaults->virtual_key(KEY_ENTER) == 0x0D, "default Enter VK");
  expect(!defaults->virtual_key(KEY_RESERVED), "reserved key ignored");
  expect(defaults->keysym(0xFFFF) == 0, "out-of-range keycode");
#ifdef LAYERC_HAVE_XKBCOMMON
  const auto us = EvdevKeymap::create(keymap_text("us"), error);
  if (!expect(us.has_value() && us->uses_xkbcommon(), "US supplied v1 keymap")) return;
  expect(us->virtual_key(KEY_Y) == 0x59, "US physical Y -> VK_Y");
  expect(us->virtual_key(KEY_Z) == 0x5A, "US physical Z -> VK_Z");
  expect(us->virtual_key(KEY_LEFTSHIFT) == 0x10, "US shift VK");
  const auto de = EvdevKeymap::create(keymap_text("de"), error);
  if (!expect(de.has_value(), "German supplied v1 keymap")) return;
  expect(de->virtual_key(KEY_Y) == 0x5A, "German physical Y -> VK_Z");
  expect(de->virtual_key(KEY_Z) == 0x59, "German physical Z -> VK_Y");
  expect(!EvdevKeymap::create(std::string("not a keymap"), error) && !error.empty(),
         "invalid supplied keymap returns visible error");
#else
  expect(!defaults->uses_xkbcommon(), "built-in fallback selected");
  expect(defaults->virtual_key(KEY_Y) == 0x59, "fallback US physical Y");
  expect(defaults->virtual_key(KEY_KP0) == 0x2D, "fallback keypad level 0 Insert");
  expect(defaults->virtual_key(KEY_KPDOT) == 0x2E, "fallback keypad level 0 Delete");
  expect(defaults->virtual_key(KEY_F24) == 0x87, "fallback F24");
  expect(defaults->virtual_key(KEY_SEMICOLON) == 0xBA, "fallback OEM punctuation");
  expect(EvdevKeymap::create(std::string("ignored without XKB"), error).has_value(),
         "supplied text still falls back when xkbcommon is not built");
#endif
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(test_keysym_table),
                 LAYERC_TEST_CASE(test_keymap_layouts))
