#include "layerc/platform/text.hpp"

#include "layerc/core/utf8.hpp"

#include <utility>

namespace layerc::platform {

std::optional<std::wstring> try_utf8_to_wide(const std::string_view text) {
  return core::try_utf8_to_wide(text);
}

std::optional<std::string> try_wide_to_utf8(const std::wstring_view text) {
  return core::try_wide_to_utf8(text);
}

std::wstring utf8_to_wide(const std::string_view text) {
  auto converted = try_utf8_to_wide(text);
  return converted.has_value() ? std::move(*converted) : std::wstring{};
}

std::string wide_to_utf8(const std::wstring_view text) {
  auto converted = try_wide_to_utf8(text);
  return converted.has_value() ? std::move(*converted) : std::string{};
}

std::optional<std::wstring>
try_invariant_lower(const std::wstring_view value) {
  const auto utf8 = core::try_wide_to_utf8(value);
  if (!utf8.has_value()) return std::nullopt;
  const auto folded = core::try_casefold_utf8(*utf8);
  return folded.has_value() ? core::try_utf8_to_wide(*folded) : std::nullopt;
}

std::wstring lower_copy(std::wstring value) {
  if (auto lowered = try_invariant_lower(value)) return std::move(*lowered);
  return value;
}

std::optional<std::string> try_casefold_path(const std::string_view value) {
  return core::try_casefold_utf8(value);
}


} // namespace layerc::platform
