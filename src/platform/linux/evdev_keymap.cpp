#include "evdev_keymap.hpp"
#include "keysym_vk.hpp"

#ifdef LAYERC_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

namespace layerc::platform {
namespace {
struct UsKey {
  std::uint16_t code;
  std::uint32_t symbol;
};
// US evdev physical positions -> unshifted XKB keysyms. Keypad navigation
// is level 0 (NumLock is not applied), just as in the XI2 adapter.
constexpr UsKey us_keys[] = {
    {KEY_A, 'a'}, {KEY_B, 'b'}, {KEY_C, 'c'}, {KEY_D, 'd'},
    {KEY_E, 'e'}, {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'},
    {KEY_I, 'i'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'},
    {KEY_M, 'm'}, {KEY_N, 'n'}, {KEY_O, 'o'}, {KEY_P, 'p'},
    {KEY_Q, 'q'}, {KEY_R, 'r'}, {KEY_S, 's'}, {KEY_T, 't'},
    {KEY_U, 'u'}, {KEY_V, 'v'}, {KEY_W, 'w'}, {KEY_X, 'x'},
    {KEY_Y, 'y'}, {KEY_Z, 'z'},
    {KEY_0, '0'}, {KEY_1, '1'}, {KEY_2, '2'}, {KEY_3, '3'},
    {KEY_4, '4'}, {KEY_5, '5'}, {KEY_6, '6'}, {KEY_7, '7'},
    {KEY_8, '8'}, {KEY_9, '9'},
    {KEY_ESC, 0xFF1B}, {KEY_TAB, 0xFF09}, {KEY_ENTER, 0xFF0D},
    {KEY_KPENTER, 0xFF8D}, {KEY_SPACE, 0x20}, {KEY_BACKSPACE, 0xFF08},
    {KEY_DELETE, 0xFFFF}, {KEY_INSERT, 0xFF63}, {KEY_HOME, 0xFF50},
    {KEY_END, 0xFF57}, {KEY_PAGEUP, 0xFF55}, {KEY_PAGEDOWN, 0xFF56},
    {KEY_LEFT, 0xFF51}, {KEY_UP, 0xFF52}, {KEY_RIGHT, 0xFF53},
    {KEY_DOWN, 0xFF54}, {KEY_SYSRQ, 0xFF61}, {KEY_SCROLLLOCK, 0xFF14},
    {KEY_PAUSE, 0xFF13}, {KEY_NUMLOCK, 0xFF7F}, {KEY_CAPSLOCK, 0xFFE5},
    {KEY_LEFTSHIFT, 0xFFE1}, {KEY_RIGHTSHIFT, 0xFFE2},
    {KEY_LEFTCTRL, 0xFFE3}, {KEY_RIGHTCTRL, 0xFFE4},
    {KEY_LEFTALT, 0xFFE9}, {KEY_RIGHTALT, 0xFFEA},
    {KEY_LEFTMETA, 0xFFEB}, {KEY_RIGHTMETA, 0xFFEC},
    {KEY_COMPOSE, 0xFF67}, {KEY_HELP, 0xFF6A}, {KEY_CANCEL, 0xFF69},
    {KEY_KPASTERISK, 0xFFAA}, {KEY_KPPLUS, 0xFFAB},
    {KEY_KPCOMMA, 0xFFAC}, {KEY_KPMINUS, 0xFFAD},
    {KEY_KPDOT, 0xFF9F}, {KEY_KPSLASH, 0xFFAF},
    {KEY_KP0, 0xFF9E}, {KEY_KP1, 0xFF9C}, {KEY_KP2, 0xFF99},
    {KEY_KP3, 0xFF9B}, {KEY_KP4, 0xFF96}, {KEY_KP5, 0xFF9D},
    {KEY_KP6, 0xFF98}, {KEY_KP7, 0xFF95}, {KEY_KP8, 0xFF97},
    {KEY_KP9, 0xFF9A}, {KEY_KPEQUAL, '='},
    {KEY_SEMICOLON, ';'}, {KEY_EQUAL, '='}, {KEY_COMMA, ','},
    {KEY_MINUS, '-'}, {KEY_DOT, '.'}, {KEY_SLASH, '/'}, {KEY_GRAVE, '`'},
    {KEY_LEFTBRACE, '['}, {KEY_BACKSLASH, '\\'}, {KEY_RIGHTBRACE, ']'},
    {KEY_APOSTROPHE, '\''}, {KEY_102ND, '<'},
    {KEY_F1, 0xFFBE}, {KEY_F2, 0xFFBF}, {KEY_F3, 0xFFC0},
    {KEY_F4, 0xFFC1}, {KEY_F5, 0xFFC2}, {KEY_F6, 0xFFC3},
    {KEY_F7, 0xFFC4}, {KEY_F8, 0xFFC5}, {KEY_F9, 0xFFC6},
    {KEY_F10, 0xFFC7}, {KEY_F11, 0xFFC8}, {KEY_F12, 0xFFC9},
    {KEY_F13, 0xFFCA}, {KEY_F14, 0xFFCB}, {KEY_F15, 0xFFCC},
    {KEY_F16, 0xFFCD}, {KEY_F17, 0xFFCE}, {KEY_F18, 0xFFCF},
    {KEY_F19, 0xFFD0}, {KEY_F20, 0xFFD1}, {KEY_F21, 0xFFD2},
    {KEY_F22, 0xFFD3}, {KEY_F23, 0xFFD4}, {KEY_F24, 0xFFD5},
};
} // namespace

std::optional<EvdevKeymap>
EvdevKeymap::create(const std::optional<std::string> &text, std::string &error) {
  error.clear();
  EvdevKeymap result;
#ifdef LAYERC_HAVE_XKBCOMMON
  auto *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (context == nullptr) {
    error = "evdev_keymap: cannot create XKB context";
    return std::nullopt;
  }
  // Do not log keymap contents or keystrokes through the library's logger.
  xkb_context_set_log_level(context, XKB_LOG_LEVEL_CRITICAL);
  auto *map = text ? xkb_keymap_new_from_string(context, text->c_str(),
                                               XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS)
                  : xkb_keymap_new_from_names(context, nullptr,
                                              XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref(context);
  if (map != nullptr) {
    for (std::size_t code = 0; code < result.symbols_.size(); ++code) {
      const xkb_keysym_t *symbols{};
      if (xkb_keymap_key_get_syms_by_level(
              map, static_cast<xkb_keycode_t>(code + 8U), 0, 0, &symbols) > 0)
        result.symbols_[code] = symbols[0];
    }
    xkb_keymap_unref(map);
    result.xkbcommon_ = true;
    return result;
  }
  if (text) {
    error = "evdev_keymap: invalid XKB v1 keymap text";
    return std::nullopt;
  }
#else
  (void)text;
#endif
  // Default XKB map unavailable (or library not built): retain US input.
  for (const auto &entry : us_keys) result.symbols_[entry.code] = entry.symbol;
  return result;
}

std::uint32_t EvdevKeymap::keysym(const std::uint16_t keycode) const noexcept {
  return keycode < symbols_.size() ? symbols_[keycode] : 0;
}

std::optional<std::uint16_t>
EvdevKeymap::virtual_key(const std::uint16_t keycode) const noexcept {
  return keysym_to_vk(keysym(keycode));
}
} // namespace layerc::platform
