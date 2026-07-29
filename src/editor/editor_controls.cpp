#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

#include <windows.h>
namespace layerc::editor {
std::wstring to_wide(const std::string_view text) {
  const auto converted = layerc::platform::utf8_to_wide(text);
  return !text.empty() && converted.empty() ? L"?" : converted;
}

std::string to_utf8(const std::wstring_view text) {
  return layerc::platform::wide_to_utf8(text);
}
std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};
  std::wstring output(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied = GetWindowTextW(control, output.data(), static_cast<int>(output.size()));
  output.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return output;
}

void set_text(const HWND control, const std::wstring_view text) {
  if (control != nullptr) SetWindowTextW(control, std::wstring(text).c_str());
}


std::uint32_t parse_uint(const std::wstring_view text, const std::uint32_t fallback) {
  const std::wstring value(text);
  wchar_t *end{};
  const auto parsed = wcstoul(value.c_str(), &end, 10);
  return end != nullptr && end != value.c_str() && *end == L'\0' && parsed <= UINT32_MAX
             ? static_cast<std::uint32_t>(parsed) : fallback;
}

std::wstring color_label(const wchar_t *label, const std::uint32_t value) {
  wchar_t text[64]{};
  swprintf_s(text, L"%ls  #%06X", label, value & 0x00FFFFFFU);
  return text;
}

} // namespace layerc::editor
