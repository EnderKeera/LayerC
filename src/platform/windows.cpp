#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string_view>

namespace layerc::platform {
namespace {

constexpr wchar_t settings_registry_key[] = L"Software\\LayerC";
constexpr wchar_t capture_exclusion_value[] = L"ExcludeFromCapture";
constexpr wchar_t editor_theme_value[] = L"EditorTheme";
constexpr wchar_t editor_accent_value[] = L"EditorAccent";
constexpr wchar_t editor_appearance_prefix[] = L"EditorAppearance.";
constexpr std::uint32_t default_editor_accent = 0xE0A15AU;
constexpr DWORD capture_exclusion_affinity = 0x00000011;

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

constexpr std::uint32_t all_editor_appearance_fields =
    editor_fill | editor_text | editor_border | editor_hover |
    editor_selection | editor_disabled;

std::wstring lower_copy(std::wstring value) {
  std::ranges::transform(value, value.begin(), [](const wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return value;
}

std::wstring utf8_to_wide(const std::string_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(),
                          count) == 0) {
    return {};
  }
  return output;
}

std::wstring process_path_for_window(const HWND window, DWORD &process_id) {
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == 0) {
    return {};
  }
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) {
    return {};
  }

  std::array<wchar_t, 32'768> path{};
  DWORD length = static_cast<DWORD>(path.size());
  const BOOL succeeded =
      QueryFullProcessImageNameW(process, 0, path.data(), &length);
  CloseHandle(process);
  return succeeded ? std::wstring(path.data(), length) : std::wstring{};
}

std::wstring window_text(const HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) {
    return {};
  }
  std::wstring output(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(window, output.data(), static_cast<int>(output.size()));
  output.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return output;
}

std::wstring window_class(const HWND window) {
  std::array<wchar_t, 256> value{};
  const int copied =
      GetClassNameW(window, value.data(), static_cast<int>(value.size()));
  return copied > 0
             ? std::wstring(value.data(), static_cast<std::size_t>(copied))
             : std::wstring{};
}

bool is_shell_window(const HWND window) {
  const auto klass = window_class(window);
  return klass == L"Shell_TrayWnd" || klass == L"Progman" ||
         klass == L"WorkerW";
}

bool is_eligible_window(const HWND window) {
  if (window == nullptr || !IsWindow(window) ||
      GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window) ||
      GetWindow(window, GW_OWNER) != nullptr || window == GetShellWindow() ||
      is_shell_window(window)) {
    return false;
  }
  RECT client{};
  if (!GetClientRect(window, &client)) {
    return false;
  }
  return client.right > client.left && client.bottom > client.top;
}

} // namespace

std::optional<WindowCandidate> describe_attachable_window(HWND window) {
  window = GetAncestor(window, GA_ROOT);
  if (!is_eligible_window(window)) {
    return std::nullopt;
  }

  WindowCandidate candidate;
  candidate.window = window;
  candidate.executable_path =
      process_path_for_window(window, candidate.process_id);
  if (candidate.executable_path.empty()) {
    return std::nullopt;
  }
  candidate.executable_name =
      std::filesystem::path(candidate.executable_path).filename().wstring();
  candidate.title = window_text(window);
  candidate.window_class = window_class(window);

  RECT client{};
  GetClientRect(window, &client);
  candidate.client_area =
      static_cast<std::uint64_t>(client.right - client.left) *
      static_cast<std::uint64_t>(client.bottom - client.top);
  return candidate;
}

std::vector<WindowCandidate> enumerate_attachable_windows() {
  std::vector<WindowCandidate> result;
  EnumWindows(
      [](const HWND window, const LPARAM context) -> BOOL {
        auto &output =
            *reinterpret_cast<std::vector<WindowCandidate> *>(context);
        if (auto candidate = describe_attachable_window(window)) {
          output.push_back(std::move(*candidate));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&result));
  std::ranges::sort(
      result, [](const WindowCandidate &left, const WindowCandidate &right) {
        if (left.executable_name != right.executable_name) {
          return lower_copy(left.executable_name) <
                 lower_copy(right.executable_name);
        }
        return lower_copy(left.title) < lower_copy(right.title);
      });
  return result;
}

std::optional<WindowCandidate> window_at_point(const POINT point) {
  return describe_attachable_window(WindowFromPoint(point));
}

bool profile_matches_window(const WindowCandidate &candidate,
                            const layerc::core::GameProfile &profile) {
  if (!profile.enabled || profile.game.global || candidate.executable_path.empty()) {
    return false;
  }

  const auto process_path = lower_copy(candidate.executable_path);
  const auto expected_path =
      lower_copy(utf8_to_wide(profile.game.executable_path));
  if (!expected_path.empty() && process_path != expected_path) {
    return false;
  }

  const auto expected_name =
      lower_copy(utf8_to_wide(profile.game.executable_name));
  if (!expected_name.empty() &&
      lower_copy(candidate.executable_name) != expected_name) {
    return false;
  }

  const auto expected_class =
      lower_copy(utf8_to_wide(profile.game.window_class));
  if (!expected_class.empty() &&
      lower_copy(candidate.window_class) != expected_class) {
    return false;
  }

  const auto expected_title =
      lower_copy(utf8_to_wide(profile.game.window_title));
  return expected_title.empty() ||
         lower_copy(candidate.title).find(expected_title) != std::wstring::npos;
}

bool capture_exclusion_enabled() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, settings_registry_key,
                                      capture_exclusion_value, RRF_RT_REG_DWORD,
                                      nullptr, &value, &size);
  return result != ERROR_SUCCESS || value != 0;
}

bool set_capture_exclusion_enabled(const bool enabled) {
  HKEY key{};
  const LSTATUS created =
      RegCreateKeyExW(HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const DWORD value = enabled ? 1U : 0U;
  const LSTATUS written =
      RegSetValueExW(key, capture_exclusion_value, 0, REG_DWORD,
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

EditorTheme editor_theme() {
  DWORD value = static_cast<DWORD>(EditorTheme::copper);
  DWORD size = sizeof(value);
  const LSTATUS result =
      RegGetValueW(HKEY_CURRENT_USER, settings_registry_key, editor_theme_value,
                   RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (result != ERROR_SUCCESS || !is_known_editor_theme(value)) {
    return EditorTheme::copper;
  }
  return static_cast<EditorTheme>(value);
}

std::uint32_t editor_accent() {
  DWORD value = default_editor_accent;
  DWORD size = sizeof(value);
  const LSTATUS result = RegGetValueW(HKEY_CURRENT_USER, settings_registry_key,
                                      editor_accent_value, RRF_RT_REG_DWORD,
                                      nullptr, &value, &size);
  return result == ERROR_SUCCESS ? (value & 0x00FFFFFFU)
                                 : default_editor_accent;
}

bool set_editor_accent(const std::uint32_t color) {
  const DWORD value = color & 0x00FFFFFFU;
  HKEY key{};
  const LSTATUS created =
      RegCreateKeyExW(HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS written =
      RegSetValueExW(key, editor_accent_value, 0, REG_DWORD,
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
  const LSTATUS created =
      RegCreateKeyExW(HKEY_CURRENT_USER, settings_registry_key, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS written =
      RegSetValueExW(key, editor_theme_value, 0, REG_DWORD,
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