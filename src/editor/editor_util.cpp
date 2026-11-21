#include "editor_text.hpp"
#include "editor_util.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace layerc::editor {

std::wstring to_wide(const std::string_view text) {
  const auto converted = layerc::platform::utf8_to_wide(text);
  // External diagnostics remain visible even if a provider returns bad UTF-8.
  return !text.empty() && converted.empty() ? L"?" : converted;
}

void set_text(const HWND control, const std::wstring_view text) {
  if (control != nullptr)
    SetWindowTextW(control, std::wstring(text).c_str());
}

std::optional<std::uint32_t> parse_bounded(const std::wstring_view text,
                                           const std::uint32_t minimum,
                                           const std::uint32_t maximum,
                                           const int base) {
  const auto parsed =
      layerc::platform::parse_integer<std::uint32_t>(text, base);
  if (!parsed || *parsed < minimum || *parsed > maximum)
    return std::nullopt;
  return parsed;
}

std::uint32_t parse_uint(const std::wstring_view text,
                         const std::uint32_t fallback) {
  return parse_bounded(text, 0, UINT32_MAX).value_or(fallback);
}

std::optional<layerc::core::Milliseconds>
parse_duration_ms(const std::wstring_view seconds) {
  const auto source = layerc::platform::ascii_narrow(seconds);
  if (!source)
    return std::nullopt;
  return parse_duration_seconds(*source);
}

ProfileCheckboxMetrics profile_checkbox_metrics(const UINT source_dpi) {
  const int dpi = source_dpi == 0 ? 96 : static_cast<int>(source_dpi);
  return {
      .image_size = std::max(1, MulDiv(20, dpi, 96)),
      .inset = std::max(1, MulDiv(2, dpi, 96)),
      .pen_width = std::max(1, MulDiv(2, dpi, 96)),
  };
}

int stored_text_scale() {
  const auto stored = layerc::platform::read_editor_text_scale();
  if (!stored.value)
    return 100;
  const auto percent = static_cast<int>(*stored.value);
  return std::ranges::find(text_scale_choices, percent) !=
                 text_scale_choices.end()
             ? percent
             : 100;
}

bool profile_checkbox_hit(const RECT &row, const POINT point,
                          const UINT dpi) {
  // The state-image cell at the row's left edge; the painted box sits inside
  // it. Anything to its right (the label) only selects the row.
  const auto metrics = profile_checkbox_metrics(dpi);
  return point.y >= row.top && point.y < row.bottom &&
         point.x >= row.left && point.x < row.left + metrics.image_size;
}

void set_owner_draw_checkbox(const HWND control, const bool checked) {
  if (control == nullptr)
    return;
  SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  InvalidateRect(control, nullptr, TRUE);
}

bool owner_draw_checkbox_checked(const HWND control) {
  return control != nullptr &&
         SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool recycle_profile_bundle(const std::filesystem::path &path) {
  std::vector<std::filesystem::path> candidates{
      path, layerc::platform::recovery_backup_path(path)};
  for (const auto &module : layerc::core::ProfileModuleTable::descriptors) {
    const auto sidecar = layerc::platform::sidecar_paths(path, module.kind);
    candidates.push_back(sidecar.primary);
    candidates.push_back(sidecar.backup);
    candidates.push_back(sidecar.marker);
  }
  const auto automation_state = layerc::platform::automation_state_path(path);
  candidates.push_back(automation_state);
  candidates.push_back(layerc::platform::recovery_backup_path(automation_state));

  std::wstring source;
  std::error_code error;
  for (const auto &candidate : candidates) {
    if (candidate.empty() || !std::filesystem::exists(candidate, error)) {
      error.clear();
      continue;
    }
    source += candidate.wstring();
    source.push_back(L'\0');
  }
  if (source.empty())
    return false;
  source.push_back(L'\0');
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = source.data();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
}

std::wstring profile_list_label(const layerc::core::GameProfile &profile) {
  return to_wide(profile_list_text(profile));
}

std::wstring profile_error_text(const layerc::core::ProfileError &error) {
  return to_wide(profile_error_line(error));
}

} // namespace layerc::editor
