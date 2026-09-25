#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::core {

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::wstring>
try_utf8_to_wide(std::string_view text);
[[nodiscard]] std::optional<std::string>
try_wide_to_utf8(std::wstring_view text);
// Deterministic comparison fold for UTF-8 identifiers and paths, identical
// on every platform: ASCII A-Z plus the cased Latin-1 supplement rows
// (U+00C0-U+00D6, U+00D8-U+00DE). All other codepoints compare byte-exact.
[[nodiscard]] std::optional<std::string>
try_casefold_utf8(std::string_view text);
// Generated identifier/file stem: lower-case ASCII letters and digits, one
// dash for every other run, and `fallback` when nothing remains.
[[nodiscard]] std::string slugify(std::string_view text,
                                  std::string_view fallback);

// Durable files and frames carry bytes; stored formats are text.
[[nodiscard]] inline std::span<const std::byte>
text_bytes(const std::string_view text) noexcept {
  return std::as_bytes(std::span(text.data(), text.size()));
}
[[nodiscard]] inline std::vector<std::byte>
owned_bytes(const std::string_view text) {
  const auto bytes = text_bytes(text);
  return {bytes.begin(), bytes.end()};
}
[[nodiscard]] inline std::string
as_text(const std::span<const std::byte> bytes) {
  if (bytes.empty()) return {};
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

namespace detail {

void append_utf8(std::string &output, std::uint32_t codepoint);

} // namespace detail
} // namespace layerc::core
