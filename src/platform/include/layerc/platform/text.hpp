#pragma once

#include "layerc/core/utf8.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

// Lossy UTF-8 <-> native-wide conversions (empty on invalid input); the
// fallible forms are core::try_utf8_to_wide / core::try_wide_to_utf8.
// wchar_t is UTF-16 on Windows and UTF-32 on Linux, so callers doing index
// arithmetic must branch on the platform width.
[[nodiscard]] std::wstring utf8_to_wide(std::string_view text);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text);
[[nodiscard]] std::wstring lower_copy(std::wstring value);
[[nodiscard]] std::optional<std::wstring>
try_invariant_lower(std::wstring_view value);
// Canonical path <-> UTF-8 conversions shared by the transaction engine,
// bundle packer, and store scan. Uniform failure modes: nullopt on
// undecodable paths (never exceptions); path_from_utf8 additionally rejects
// invalid UTF-8. One owner — do not reimplement per module.
[[nodiscard]] std::optional<std::string>
path_to_utf8(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::filesystem::path>
path_from_utf8(std::string_view text);

} // namespace layerc::platform
