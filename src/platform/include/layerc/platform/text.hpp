#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

[[nodiscard]] std::optional<std::wstring>
try_utf8_to_wide(std::string_view text);
[[nodiscard]] std::optional<std::string>
try_wide_to_utf8(std::wstring_view text);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view text);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text);
[[nodiscard]] std::wstring lower_copy(std::wstring value);
[[nodiscard]] std::optional<std::wstring>
try_invariant_lower(std::wstring_view value);
// Case-folded comparison key for UTF-8 paths (used for archive entry
// collision detection); nullopt when the input is not valid UTF-8.
[[nodiscard]] std::optional<std::wstring>
try_casefold_path(std::string_view value);

} // namespace layerc::platform
