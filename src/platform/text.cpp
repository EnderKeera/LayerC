#include "layerc/platform/text.hpp"

#include <limits>
#include <utility>

#include <windows.h>

namespace layerc::platform {

std::optional<std::wstring> try_utf8_to_wide(const std::string_view text) {
  if (text.empty()) return std::wstring{};
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (count <= 0) {
    return std::nullopt;
  }
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count) ==
      0) {
    return std::nullopt;
  }
  return output;
}

std::wstring utf8_to_wide(const std::string_view text) {
  auto converted = try_utf8_to_wide(text);
  return converted.has_value() ? std::move(*converted) : std::wstring{};
}

std::optional<std::string> try_wide_to_utf8(const std::wstring_view text) {
  if (text.empty()) return std::string{};
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0) {
    return std::nullopt;
  }
  std::string output(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count,
                          nullptr, nullptr) == 0) {
    return std::nullopt;
  }
  return output;
}

std::string wide_to_utf8(const std::wstring_view text) {
  auto converted = try_wide_to_utf8(text);
  return converted.has_value() ? std::move(*converted) : std::string{};
}

std::optional<std::wstring>
try_invariant_lower(const std::wstring_view value) {
  if (value.empty()) return std::wstring{};
  if (value.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int required = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
  if (required <= 0) return std::nullopt;
  std::wstring lowered(static_cast<std::size_t>(required), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                    static_cast<int>(value.size()), lowered.data(), required,
                    nullptr, nullptr, 0) != required)
    return std::nullopt;
  return lowered;
}

std::wstring lower_copy(std::wstring value) {
  if (value.empty()) return value;
  if (auto lowered = try_invariant_lower(value)) return std::move(*lowered);
  CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
  return value;
}

std::optional<std::wstring>
try_casefold_path(const std::string_view value) {
  auto wide = try_utf8_to_wide(value);
  if (!wide.has_value())
    return std::nullopt;
  return try_invariant_lower(*wide);
}

} // namespace layerc::platform
