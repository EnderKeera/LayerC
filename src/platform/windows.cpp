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
constexpr wchar_t editor_appearance_value[] = L"EditorAppearance";
constexpr wchar_t editor_theme_value[] = L"EditorTheme";
constexpr wchar_t editor_accent_value[] = L"EditorAccent";
constexpr wchar_t editor_appearance_prefix[] = L"EditorAppearance.";
constexpr DWORD capture_exclusion_affinity = 0x00000011;
constexpr std::uint32_t editor_appearance_version = 1;

struct StoredEditorAppearance {
  std::uint32_t version{};
  std::uint32_t theme{};
  std::uint32_t accent{};
  std::uint32_t reserved{};
  std::array<EditorAppearanceOverrides,
             static_cast<std::size_t>(EditorControlRole::count)>
      overrides{};
};
constexpr std::uint32_t all_editor_appearance_fields =
    editor_fill | editor_text | editor_border | editor_hover |
    editor_selection | editor_disabled;

bool is_known_editor_theme(const DWORD value) {
  return value <= static_cast<DWORD>(EditorTheme::dark);
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

EditorAppearanceOverrides
read_legacy_editor_overrides(const EditorControlRole role) {
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
  if (result != ERROR_SUCCESS || size != sizeof(overrides))
    return {};
  overrides.fields &= all_editor_appearance_fields;
  return overrides;
}

EditorAppearanceSettings legacy_editor_appearance() {
  EditorAppearanceSettings settings;
  if (const auto theme = read_dword(editor_theme_value);
      theme.has_value() &&
      *theme <= static_cast<DWORD>(EditorTheme::plum)) {
    settings.theme = static_cast<EditorTheme>(*theme);
  }
  if (const auto accent = read_dword(editor_accent_value); accent.has_value())
    settings.accent = *accent & 0x00FFFFFFU;
  for (std::size_t index = 0; index < settings.overrides.size(); ++index)
    settings.overrides[index] = read_legacy_editor_overrides(
        static_cast<EditorControlRole>(index));
  return settings;
}

std::optional<EditorAppearanceSettings> read_editor_appearance() {
  StoredEditorAppearance stored{};
  DWORD size = sizeof(stored);
  const HKEY key = open_layerc_key(KEY_QUERY_VALUE, false);
  if (key == nullptr)
    return std::nullopt;
  const LSTATUS result =
      RegGetValueW(key, nullptr, editor_appearance_value, RRF_RT_REG_BINARY,
                   nullptr, &stored, &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || size != sizeof(stored) ||
      stored.version != editor_appearance_version ||
      !is_known_editor_theme(stored.theme))
    return std::nullopt;

  EditorAppearanceSettings settings{
      .theme = static_cast<EditorTheme>(stored.theme),
      .accent = stored.accent & 0x00FFFFFFU,
      .overrides = stored.overrides,
  };
  for (auto &overrides : settings.overrides)
    overrides.fields &= all_editor_appearance_fields;
  return settings;
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

bool same_user_as_current_process(const HANDLE process) {
  HANDLE current_token{};
  HANDLE other_token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token) ==
          FALSE ||
      OpenProcessToken(process, TOKEN_QUERY, &other_token) == FALSE) {
    if (current_token != nullptr) CloseHandle(current_token);
    if (other_token != nullptr) CloseHandle(other_token);
    return false;
  }
  const auto token_user = [](const HANDLE token,
                             std::vector<std::byte> &storage) -> PSID {
    DWORD required{};
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      return nullptr;
    storage.resize(required);
    if (GetTokenInformation(token, TokenUser, storage.data(), required,
                            &required) == FALSE)
      return nullptr;
    return reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid;
  };
  std::vector<std::byte> current_storage;
  std::vector<std::byte> other_storage;
  const PSID current_sid = token_user(current_token, current_storage);
  const PSID other_sid = token_user(other_token, other_storage);
  const bool matches = current_sid != nullptr && other_sid != nullptr &&
                       EqualSid(current_sid, other_sid) != FALSE;
  CloseHandle(other_token);
  CloseHandle(current_token);
  return matches;
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

EditorAppearanceSettings editor_appearance() {
  const auto settings = read_editor_appearance();
  if (settings.has_value())
    return *settings;
  // One-time migration: write the versioned blob back so the per-value legacy
  // reads below never run again on this machine (barring registry failure).
  const auto legacy = legacy_editor_appearance();
  (void)set_editor_appearance(legacy);
  return legacy;
}

bool set_editor_appearance(const EditorAppearanceSettings &settings) {
  const auto theme = static_cast<std::uint32_t>(settings.theme);
  if (!is_known_editor_theme(theme))
    return false;

  StoredEditorAppearance stored{
      .version = editor_appearance_version,
      .theme = theme,
      .accent = settings.accent & 0x00FFFFFFU,
      .overrides = settings.overrides,
  };
  for (auto &overrides : stored.overrides)
    overrides.fields &= all_editor_appearance_fields;

  const HKEY key = open_layerc_key(KEY_SET_VALUE, true);
  if (key == nullptr)
    return false;
  const LSTATUS written = RegSetValueExW(
      key, editor_appearance_value, 0, REG_BINARY,
      reinterpret_cast<const BYTE *>(&stored), sizeof(stored));
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

EditorTheme editor_theme() { return editor_appearance().theme; }

std::uint32_t editor_accent() { return editor_appearance().accent; }

bool set_editor_theme(const EditorTheme theme) {
  auto settings = editor_appearance();
  settings.theme = theme;
  return set_editor_appearance(settings);
}

EditorAppearanceOverrides
editor_appearance_overrides(const EditorControlRole role) {
  if (!is_known_editor_control_role(role))
    return {};
  return editor_appearance().overrides[static_cast<std::size_t>(role)];
}

bool set_editor_appearance_overrides(
    const EditorControlRole role,
    const EditorAppearanceOverrides &source_overrides) {
  if (!is_known_editor_control_role(role))
    return false;
  auto settings = editor_appearance();
  settings.overrides[static_cast<std::size_t>(role)] = source_overrides;
  return set_editor_appearance(settings);
}

bool reset_editor_appearance_overrides(const EditorControlRole role,
                                       const std::uint32_t fields) {
  if (!is_known_editor_control_role(role))
    return false;
  auto settings = editor_appearance();
  settings.overrides[static_cast<std::size_t>(role)].fields &=
      ~(fields & all_editor_appearance_fields);
  return set_editor_appearance(settings);
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
