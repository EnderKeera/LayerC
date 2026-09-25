#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

[[nodiscard]] std::wstring to_wide(std::string_view text);
[[nodiscard]] std::string to_utf8(std::wstring_view text);
[[nodiscard]] std::wstring control_text(HWND control);
void set_text(HWND control, std::wstring_view text);
// Digits only: signs, whitespace, prefixes and out-of-range values are
// rejected, so "-1" or "-FFFFF" can never wrap into a large unsigned value.
[[nodiscard]] std::optional<std::uint32_t>
parse_bounded(std::wstring_view text, std::uint32_t minimum,
              std::uint32_t maximum, int base = 10);
[[nodiscard]] std::uint32_t parse_uint(std::wstring_view text,
                                       std::uint32_t fallback);
[[nodiscard]] std::optional<layerc::core::Milliseconds>
parse_duration_ms(std::wstring_view seconds);
[[nodiscard]] std::wstring
duration_seconds_text(layerc::core::Milliseconds duration);

} // namespace layerc::editor
