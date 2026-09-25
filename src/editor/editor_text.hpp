#pragma once

#include "layerc/core/types.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/window_query.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

using layerc::platform::wide_to_utf8;
using layerc::platform::window_text;

// Like platform::utf8_to_wide, but shows "?" for undecodable non-empty text.
[[nodiscard]] std::wstring to_wide(std::string_view text);
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
