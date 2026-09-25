#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

// UTF-8 <-> native-wide conversions. Width variance warning: wchar_t is
// UTF-16 on Windows and UTF-32 on Linux, so try_utf8_to_wide yields
// surrogate pairs per unit on Windows but single units on Linux. Callers
// doing index arithmetic or casts must branch on the platform width;
// equality/compare helpers (try_casefold_path) already hide the split.
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
[[nodiscard]] std::optional<std::string>
try_casefold_path(std::string_view value);
// Canonical path <-> UTF-8 conversions shared by the transaction engine,
// bundle packer, and store scan. Uniform failure modes: nullopt on
// undecodable paths (never exceptions); path_from_utf8 additionally rejects
// invalid UTF-8. One owner — do not reimplement per module.
[[nodiscard]] std::optional<std::string>
path_to_utf8(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::filesystem::path>
path_from_utf8(std::string_view text);

} // namespace layerc::platform
