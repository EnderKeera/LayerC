#include "layerc/platform/windows.hpp"

#include <appmodel.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <shellscalingapi.h>

namespace layerc::platform {
namespace {

constexpr wchar_t settings_registry_key[] = L"Software\\LayerC";
constexpr wchar_t capture_exclusion_value[] = L"ExcludeFromCapture";
constexpr wchar_t visual_recognition_value[] = L"VisualRecognitionEnabled";
constexpr wchar_t visual_capture_notice_value[] = L"VisualCaptureNoticeSeen";
constexpr wchar_t editor_theme_value[] = L"EditorTheme";
constexpr wchar_t editor_accent_value[] = L"EditorAccent";
constexpr wchar_t editor_appearance_prefix[] = L"EditorAppearance.";
constexpr std::uint32_t default_editor_accent = 0xE0A15AU;
constexpr DWORD capture_exclusion_affinity = 0x00000011;
constexpr std::uint32_t all_editor_appearance_fields =
    editor_fill | editor_text | editor_border | editor_hover |
    editor_selection | editor_disabled;

bool is_known_editor_theme(const DWORD value) {
  return value <= static_cast<DWORD>(EditorTheme::plum);
}

bool is_known_editor_control_role(const EditorControlRole role) {
  return static_cast<std::uint32_t>(role) <
         static_cast<std::uint32_t>(EditorControlRole::count);
}

std::wstring editor_appearance_value_name(const EditorControlRole role) {
  return std::wstring(editor_appearance_prefix) +
         std::to_wstring(static_cast<std::uint32_t>(role));
}

HKEY open_layerc_key(const REGSAM access, const bool create) {
  HKEY key{};
  const LSTATUS result =
      create ? RegCreateKeyExW(HKEY_CURRENT_USER, settings_registry_key, 0,
                               nullptr, 0, access, nullptr, &key, nullptr)
             : RegOpenKeyExW(HKEY_CURRENT_USER, settings_registry_key, 0,
                             access, &key);
  return result == ERROR_SUCCESS ? key : nullptr;
}

std::optional<DWORD> read_dword(const wchar_t *name) {
  const HKEY key = open_layerc_key(KEY_QUERY_VALUE, false);
  if (key == nullptr)
    return std::nullopt;
  DWORD value{};
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD,
                                      nullptr, &value, &size);
  RegCloseKey(key);
  return result == ERROR_SUCCESS ? std::optional<DWORD>(value) : std::nullopt;
}

bool read_default_true_dword(const wchar_t *name) {
  const auto value = read_dword(name);
  return !value.has_value() || *value != 0;
}

bool write_dword(const wchar_t *name, const DWORD value) {
  const HKEY key = open_layerc_key(KEY_SET_VALUE, true);
  if (key == nullptr)
    return false;
  const auto written =
      RegSetValueExW(key, name, 0, REG_DWORD,
                     reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

bool write_bool_dword(const wchar_t *name, const bool enabled) {
  return write_dword(name, enabled ? 1U : 0U);
}

} // namespace

std::wstring windows_error_text(const DWORD error) {
  if (error == ERROR_SUCCESS)
    return {};
  wchar_t *message{};
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  std::wstring result = length != 0 && message != nullptr
                            ? std::wstring(message, length)
                            : L"Windows error " + std::to_wstring(error);
  if (message != nullptr)
    LocalFree(message);
  while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                             result.back() == L' '))
    result.pop_back();
  return result;
}

bool capture_exclusion_enabled() {
  return read_default_true_dword(capture_exclusion_value);
}

bool set_capture_exclusion_enabled(const bool enabled) {
  return write_bool_dword(capture_exclusion_value, enabled);
}

bool visual_recognition_enabled() {
  return read_default_true_dword(visual_recognition_value);
}

bool set_visual_recognition_enabled(const bool enabled) {
  return write_bool_dword(visual_recognition_value, enabled);
}

bool visual_capture_notice_acknowledged() {
  const auto value = read_dword(visual_capture_notice_value);
  return value.has_value() && *value != 0;
}

bool set_visual_capture_notice_acknowledged(const bool acknowledged) {
  return write_bool_dword(visual_capture_notice_value, acknowledged);
}

bool apply_capture_exclusion(const HWND window, const bool enabled) {
  if (window == nullptr || !IsWindow(window)) {
    return false;
  }
  return SetWindowDisplayAffinity(window, enabled ? capture_exclusion_affinity
                                                  : WDA_NONE) != FALSE;
}

UINT monitor_dpi_for_rect(const RECT &rect) {
  const HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
  UINT dpi_x{};
  UINT dpi_y{};
  if (monitor != nullptr &&
      SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) &&
      dpi_x != 0) {
    return dpi_x;
  }
  const UINT system_dpi = GetDpiForSystem();
  return system_dpi == 0 ? 96U : system_dpi;
}

EditorTheme editor_theme() {
  const auto value = read_dword(editor_theme_value);
  if (!value.has_value() || !is_known_editor_theme(*value)) {
    return EditorTheme::copper;
  }
  return static_cast<EditorTheme>(*value);
}

std::uint32_t editor_accent() {
  const auto value = read_dword(editor_accent_value);
  return value.has_value() ? (*value & 0x00FFFFFFU) : default_editor_accent;
}

bool set_editor_accent(const std::uint32_t color) {
  const DWORD value = color & 0x00FFFFFFU;
  return write_dword(editor_accent_value, value);
}

bool set_editor_theme(const EditorTheme theme) {
  const DWORD value = static_cast<DWORD>(theme);
  if (!is_known_editor_theme(value)) {
    return false;
  }
  return write_dword(editor_theme_value, value);
}

EditorAppearanceOverrides
editor_appearance_overrides(const EditorControlRole role) {
  if (!is_known_editor_control_role(role)) {
    return {};
  }
  EditorAppearanceOverrides overrides{};
  DWORD size = sizeof(overrides);
  const auto value_name = editor_appearance_value_name(role);
  const HKEY key = open_layerc_key(KEY_QUERY_VALUE, false);
  if (key == nullptr)
    return {};
  const LSTATUS result =
      RegGetValueW(key, nullptr, value_name.c_str(), RRF_RT_REG_BINARY, nullptr,
                   &overrides, &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || size != sizeof(overrides)) {
    return {};
  }
  overrides.fields &= all_editor_appearance_fields;
  return overrides;
}

bool set_editor_appearance_overrides(
    const EditorControlRole role,
    const EditorAppearanceOverrides &source_overrides) {
  if (!is_known_editor_control_role(role)) {
    return false;
  }
  EditorAppearanceOverrides overrides = source_overrides;
  overrides.fields &= all_editor_appearance_fields;
  const auto value_name = editor_appearance_value_name(role);
  const HKEY key = open_layerc_key(KEY_SET_VALUE, true);
  if (key == nullptr)
    return false;
  LSTATUS written = ERROR_SUCCESS;
  if (overrides.fields == 0) {
    written = RegDeleteValueW(key, value_name.c_str());
    if (written == ERROR_FILE_NOT_FOUND) {
      written = ERROR_SUCCESS;
    }
  } else {
    written = RegSetValueExW(key, value_name.c_str(), 0, REG_BINARY,
                             reinterpret_cast<const BYTE *>(&overrides),
                             sizeof(overrides));
  }
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

bool reset_editor_appearance_overrides(const EditorControlRole role,
                                       const std::uint32_t fields) {
  auto overrides = editor_appearance_overrides(role);
  overrides.fields &= ~(fields & all_editor_appearance_fields);
  return set_editor_appearance_overrides(role, overrides);
}

bool signal_settings_reload() {
  const HANDLE event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, settings_reload_event_name);
  if (event == nullptr) {
    return false;
  }
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}
bool is_gamebar_overlay_process(const DWORD process_id) {
  if (process_id == 0)
    return false;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr)
    return false;

  UINT32 family_length{};
  const LONG probe = GetPackageFamilyName(process, &family_length, nullptr);
  if (probe != ERROR_INSUFFICIENT_BUFFER || family_length <= 1U) {
    CloseHandle(process);
    return false;
  }
  std::vector<wchar_t> family(family_length);
  const LONG result =
      GetPackageFamilyName(process, &family_length, family.data());
  CloseHandle(process);
  return result == ERROR_SUCCESS &&
         std::wstring_view(family.data()) ==
             L"Microsoft.XboxGamingOverlay_8wekyb3d8bbwe";
}
std::string window_session_key(const HWND window) {
  if (window == nullptr)
    return "global";
  return "window:" + std::to_string(reinterpret_cast<std::uintptr_t>(window));
}

} // namespace layerc::platform
