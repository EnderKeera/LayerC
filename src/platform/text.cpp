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

std::string wide_to_utf8(const std::wstring_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0) {
    return {};
  }
  std::string output(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count,
                          nullptr, nullptr) == 0) {
    return {};
  }
  return output;
}

std::wstring lower_copy(std::wstring value) {
  if (value.empty()) return value;
  const int required = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
  if (required > 0) {
    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), lowered.data(), required,
                      nullptr, nullptr, 0) != 0)
      return lowered;
  }
  CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
  return value;
}

} // namespace layerc::platform
