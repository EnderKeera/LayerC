#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <string_view>

#include <windows.h>
namespace layerc::editor {
std::wstring to_wide(const std::string_view text) {
  const auto converted = layerc::platform::utf8_to_wide(text);
  // External diagnostics remain visible even if a provider returns bad UTF-8.
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

HFONT create_editor_font(const UINT dpi) {
  const auto effective_dpi = dpi == 0 ? 96 : static_cast<int>(dpi);
  return CreateFontW(-MulDiv(9, effective_dpi, 72), 0, 0, 0, FW_NORMAL,
                     FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
}

void apply_suggested_dpi_rect(const HWND window, const LPARAM lparam) {
  const auto *suggested = reinterpret_cast<const RECT *>(lparam);
  if (suggested == nullptr) return;
  SetWindowPos(window, nullptr, suggested->left, suggested->top,
               suggested->right - suggested->left,
               suggested->bottom - suggested->top,
               SWP_NOZORDER | SWP_NOACTIVATE);
}


std::uint32_t parse_uint(const std::wstring_view text, const std::uint32_t fallback) {
  const std::wstring value(text);
  wchar_t *end{};
  const auto parsed = wcstoul(value.c_str(), &end, 10);
  return end != nullptr && end != value.c_str() && *end == L'\0' && parsed <= UINT32_MAX
             ? static_cast<std::uint32_t>(parsed) : fallback;
}

std::optional<layerc::core::Milliseconds>
parse_duration_ms(const std::wstring_view seconds) {
  if (seconds.empty())
    return std::nullopt;
  const std::wstring value(seconds);
  wchar_t *end{};
  const double parsed = wcstod(value.c_str(), &end);
  if (end == value.c_str() || end == nullptr || *end != L'\0' ||
      !std::isfinite(parsed) || parsed <= 0.0 || parsed > 86'400.0)
    return std::nullopt;
  const auto milliseconds = std::llround(parsed * 1000.0);
  if (milliseconds < 1 || milliseconds > 86'400'000)
    return std::nullopt;
  return layerc::core::Milliseconds(milliseconds);
}

} // namespace layerc::editor
