#pragma once

#include "layerc/core/utf8.hpp"

#include <charconv>
#include <concepts>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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

// std::from_chars has no wide overload. Numeric text is ASCII, so narrowing is
// exact once every non-ASCII code unit has been rejected.
[[nodiscard]] inline std::optional<std::string>
ascii_narrow(const std::wstring_view text) {
  std::string narrow;
  narrow.reserve(text.size());
  for (const wchar_t character : text) {
    if (character > 0x7F) return std::nullopt;
    narrow.push_back(static_cast<char>(character));
  }
  return narrow;
}

// Whole-string integer in `base`: no sign for unsigned types, no whitespace.
template <std::integral Integer>
[[nodiscard]] std::optional<Integer>
parse_integer(const std::wstring_view text, const int base = 10) {
  const auto narrow = ascii_narrow(text);
  if (!narrow.has_value() || narrow->empty()) return std::nullopt;
  const auto *const last = narrow->data() + narrow->size();
  Integer value{};
  const auto [end, error] = std::from_chars(narrow->data(), last, value, base);
  if (error != std::errc{} || end != last) return std::nullopt;
  return value;
}

} // namespace layerc::platform
