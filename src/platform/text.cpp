#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>

#include <windows.h>

namespace layerc::platform {

std::wstring utf8_to_wide(const std::string_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count) ==
      0) {
    return {};
  }
  return output;
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
  std::ranges::transform(value, value.begin(), [](const wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return value;
}

} // namespace layerc::platform