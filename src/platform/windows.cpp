#include "layerc/platform/windows.hpp"

#include <string>

#include <shellscalingapi.h>

namespace layerc::platform {
namespace {

constexpr wchar_t settings_registry_key[] = L"Software\\LayerC";
constexpr wchar_t capture_exclusion_value[] = L"ExcludeFromCapture";
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

} // namespace

bool capture_exclusion_enabled() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(
      HKEY_CURRENT_USER, settings_registry_key, capture_exclusion_value,
      RRF_RT_REG_DWORD, nullptr, &value, &size);
  return result != ERROR_SUCCESS || value != 0;
}

bool set_capture_exclusion_enabled(const bool enabled) {
  HKEY key{};
  const LSTATUS created = RegCreateKeyExW(
      HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0, KEY_SET_VALUE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const DWORD value = enabled ? 1U : 0U;
  const LSTATUS written = RegSetValueExW(
      key, capture_exclusion_value, 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

bool apply_capture_exclusion(const HWND window, const bool enabled) {
  if (window == nullptr || !IsWindow(window)) {
    return false;
  }
  return SetWindowDisplayAffinity(window, enabled ? capture_exclusion_affinity
                                                  : WDA_NONE) != FALSE;
}

UINT monitor_dpi_for_rect(const RECT &rect) {
  const HMONITOR monitor =
      MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
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
  DWORD value = static_cast<DWORD>(EditorTheme::copper);
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(
      HKEY_CURRENT_USER, settings_registry_key, editor_theme_value,
      RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (result != ERROR_SUCCESS || !is_known_editor_theme(value)) {
    return EditorTheme::copper;
  }
  return static_cast<EditorTheme>(value);
}

std::uint32_t editor_accent() {
  DWORD value = default_editor_accent;
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(
      HKEY_CURRENT_USER, settings_registry_key, editor_accent_value,
      RRF_RT_REG_DWORD, nullptr, &value, &size);
  return result == ERROR_SUCCESS ? (value & 0x00FFFFFFU)
                                 : default_editor_accent;
}

bool set_editor_accent(const std::uint32_t color) {
  const DWORD value = color & 0x00FFFFFFU;
  HKEY key{};
  const LSTATUS created = RegCreateKeyExW(
      HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0, KEY_SET_VALUE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS written = RegSetValueExW(
      key, editor_accent_value, 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

bool set_editor_theme(const EditorTheme theme) {
  const DWORD value = static_cast<DWORD>(theme);
  if (!is_known_editor_theme(value)) {
    return false;
  }
  HKEY key{};
  const LSTATUS created = RegCreateKeyExW(
      HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0, KEY_SET_VALUE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS written = RegSetValueExW(
      key, editor_theme_value, 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

EditorAppearanceOverrides
editor_appearance_overrides(const EditorControlRole role) {
  if (!is_known_editor_control_role(role)) {
    return {};
  }
  EditorAppearanceOverrides overrides{};
  DWORD size = sizeof(overrides);
  const auto value_name = editor_appearance_value_name(role);
  const LSTATUS result = RegGetValueW(
      HKEY_CURRENT_USER, settings_registry_key, value_name.c_str(),
      RRF_RT_REG_BINARY, nullptr, &overrides, &size);
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
  HKEY key{};
  const LSTATUS created = RegCreateKeyExW(
      HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0, KEY_SET_VALUE,
      nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  LSTATUS written = ERROR_SUCCESS;
  if (overrides.fields == 0) {
    written = RegDeleteValueW(key, value_name.c_str());
    if (written == ERROR_FILE_NOT_FOUND) {
      written = ERROR_SUCCESS;
    }
  } else {
    written = RegSetValueExW(
        key, value_name.c_str(), 0, REG_BINARY,
        reinterpret_cast<const BYTE *>(&overrides), sizeof(overrides));
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

} // namespace layerc::platform
