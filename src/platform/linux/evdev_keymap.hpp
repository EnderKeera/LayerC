#pragma once

#include <linux/input-event-codes.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace layerc::platform {

// Immutable group-0, level-0 map. No modifier state or layout switching:
// matches XI2 XkbKeycodeToKeysym(..., 0, 0). Construction releases XKB objects.
class EvdevKeymap {
public:
  // With xkbcommon: supplied Wayland v1 keymap text or default RMLVO names.
  // Without it: built-in US map, even if text is supplied. Malformed supplied
  // text is a creation error when xkbcommon is available (never silently US).
  [[nodiscard]] static std::optional<EvdevKeymap>
  create(const std::optional<std::string> &text, std::string &error);
  [[nodiscard]] std::uint32_t keysym(std::uint16_t keycode) const noexcept;
  [[nodiscard]] std::optional<std::uint16_t>
  virtual_key(std::uint16_t keycode) const noexcept;
  [[nodiscard]] bool uses_xkbcommon() const noexcept { return xkbcommon_; }

private:
  std::array<std::uint32_t, KEY_CNT> symbols_{};
  bool xkbcommon_{};
};

} // namespace layerc::platform
