#include "layerc/platform/settings_store.hpp"

#include <limits>
#include <utility>

#include <windows.h>

namespace layerc::platform {
namespace {

constexpr wchar_t settings_registry_key[] = L"Software\\LayerC";
constexpr wchar_t capture_exclusion_value[] = L"ExcludeFromCapture";
constexpr wchar_t visual_recognition_value[] = L"VisualRecognitionEnabled";
constexpr wchar_t visual_capture_notice_value[] = L"VisualCaptureNoticeSeen";

struct OpenKeyResult {
  HKEY key{};
  LSTATUS status{ERROR_SUCCESS};
};

bool is_missing(const LSTATUS status) {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

std::wstring registry_path(const std::wstring_view subkey) {
  std::wstring path(settings_registry_key);
  if (!subkey.empty()) {
    path.push_back(L'\\');
    path.append(subkey);
  }
  return path;
}

OpenKeyResult open_layerc_key(const std::wstring_view subkey,
                              const REGSAM access, const bool create) {
  HKEY key{};
  const auto path = registry_path(subkey);
  const LSTATUS status =
      create ? RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                               access, nullptr, &key, nullptr)
             : RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, &key);
  return {.key = status == ERROR_SUCCESS ? key : nullptr, .status = status};
}

template <typename T>
SettingReadResult<T> read_error(const LSTATUS status) {
  return {.value = std::nullopt,
          .error = is_missing(status) ? 0U
                                      : static_cast<std::uint32_t>(status)};
}

SettingReadResult<bool> read_bool(const wchar_t *const name) {
  const auto value = read_layerc_registry_dword(name);
  if (!value.value)
    return {.value = std::nullopt, .error = value.error};
  return {.value = *value.value != 0, .error = 0};
}

} // namespace

SettingReadResult<std::uint32_t>
read_layerc_registry_dword(const std::wstring_view value_name,
                           const std::wstring_view subkey) {
  const auto opened = open_layerc_key(subkey, KEY_QUERY_VALUE, false);
  if (opened.key == nullptr)
    return read_error<std::uint32_t>(opened.status);

  const std::wstring name(value_name);
  DWORD value{};
  DWORD size = sizeof(value);
  const LSTATUS status =
      RegGetValueW(opened.key, nullptr, name.c_str(), RRF_RT_REG_DWORD, nullptr,
                   &value, &size);
  RegCloseKey(opened.key);
  if (status != ERROR_SUCCESS)
    return read_error<std::uint32_t>(status);
  return {.value = value, .error = 0};
}

SettingWriteResult
write_layerc_registry_dword(const std::wstring_view value_name,
                            const std::uint32_t value,
                            const std::wstring_view subkey) {
  const auto opened = open_layerc_key(subkey, KEY_SET_VALUE, true);
  if (opened.key == nullptr)
    return {.error = static_cast<std::uint32_t>(opened.status)};
  const std::wstring name(value_name);
  const LSTATUS status = RegSetValueExW(
      opened.key, name.c_str(), 0, REG_DWORD,
      reinterpret_cast<const BYTE *>(&value), sizeof(value));
  RegCloseKey(opened.key);
  return {.error = static_cast<std::uint32_t>(status)};
}

SettingReadResult<std::vector<std::byte>>
read_layerc_registry_binary(const std::wstring_view value_name,
                            const std::size_t maximum_bytes,
                            const std::wstring_view subkey) {
  const auto opened = open_layerc_key(subkey, KEY_QUERY_VALUE, false);
  if (opened.key == nullptr)
    return read_error<std::vector<std::byte>>(opened.status);
  const std::wstring name(value_name);
  DWORD size{};
  LSTATUS status =
      RegGetValueW(opened.key, nullptr, name.c_str(), RRF_RT_REG_BINARY, nullptr,
                   nullptr, &size);
  if (status != ERROR_SUCCESS) {
    RegCloseKey(opened.key);
    return read_error<std::vector<std::byte>>(status);
  }
  if (size > maximum_bytes) {
    RegCloseKey(opened.key);
    return {.value = std::nullopt, .error = ERROR_FILE_TOO_LARGE};
  }
  std::vector<std::byte> value(size);
  status = RegGetValueW(opened.key, nullptr, name.c_str(), RRF_RT_REG_BINARY,
                        nullptr, value.data(), &size);
  RegCloseKey(opened.key);
  if (status != ERROR_SUCCESS)
    return read_error<std::vector<std::byte>>(status);
  value.resize(size);
  return {.value = std::move(value), .error = 0};
}

SettingWriteResult
write_layerc_registry_binary(const std::wstring_view value_name,
                             const std::span<const std::byte> value,
                             const std::wstring_view subkey) {
  if (value.size() > std::numeric_limits<DWORD>::max())
    return {.error = ERROR_FILE_TOO_LARGE};
  const auto opened = open_layerc_key(subkey, KEY_SET_VALUE, true);
  if (opened.key == nullptr)
    return {.error = static_cast<std::uint32_t>(opened.status)};
  const std::wstring name(value_name);
  const LSTATUS status = RegSetValueExW(
      opened.key, name.c_str(), 0, REG_BINARY,
      reinterpret_cast<const BYTE *>(value.data()),
      static_cast<DWORD>(value.size()));
  RegCloseKey(opened.key);
  return {.error = static_cast<std::uint32_t>(status)};
}

SettingReadResult<std::wstring>
read_layerc_registry_string(const std::wstring_view value_name,
                            const std::size_t maximum_characters,
                            const std::wstring_view subkey) {
  const auto opened = open_layerc_key(subkey, KEY_QUERY_VALUE, false);
  if (opened.key == nullptr)
    return read_error<std::wstring>(opened.status);
  const std::wstring name(value_name);
  DWORD bytes{};
  LSTATUS status =
      RegGetValueW(opened.key, nullptr, name.c_str(), RRF_RT_REG_SZ, nullptr,
                   nullptr, &bytes);
  if (status != ERROR_SUCCESS) {
    RegCloseKey(opened.key);
    return read_error<std::wstring>(status);
  }
  if (bytes % sizeof(wchar_t) != 0 ||
      bytes / sizeof(wchar_t) > maximum_characters + 1U) {
    RegCloseKey(opened.key);
    return {.value = std::nullopt, .error = ERROR_INVALID_DATA};
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  status = RegGetValueW(opened.key, nullptr, name.c_str(), RRF_RT_REG_SZ,
                        nullptr, value.data(), &bytes);
  RegCloseKey(opened.key);
  if (status != ERROR_SUCCESS)
    return read_error<std::wstring>(status);
  while (!value.empty() && value.back() == L'\0')
    value.pop_back();
  return {.value = std::move(value), .error = 0};
}

SettingWriteResult
write_layerc_registry_string(const std::wstring_view value_name,
                             const std::wstring_view value,
                             const std::wstring_view subkey) {
  const auto characters = value.size() + 1U;
  if (characters > std::numeric_limits<DWORD>::max() / sizeof(wchar_t))
    return {.error = ERROR_FILE_TOO_LARGE};
  const auto opened = open_layerc_key(subkey, KEY_SET_VALUE, true);
  if (opened.key == nullptr)
    return {.error = static_cast<std::uint32_t>(opened.status)};
  const std::wstring name(value_name);
  const std::wstring stored(value);
  const DWORD bytes = static_cast<DWORD>(characters * sizeof(wchar_t));
  const LSTATUS status = RegSetValueExW(
      opened.key, name.c_str(), 0, REG_SZ,
      reinterpret_cast<const BYTE *>(stored.c_str()), bytes);
  RegCloseKey(opened.key);
  return {.error = static_cast<std::uint32_t>(status)};
}

SettingReadResult<bool> read_capture_exclusion_setting() {
  return read_bool(capture_exclusion_value);
}

SettingWriteResult write_capture_exclusion_setting(const bool enabled) {
  return write_layerc_registry_dword(capture_exclusion_value, enabled ? 1U : 0U);
}

SettingReadResult<bool> read_visual_recognition_setting() {
  return read_bool(visual_recognition_value);
}

SettingWriteResult write_visual_recognition_setting(const bool enabled) {
  return write_layerc_registry_dword(visual_recognition_value, enabled ? 1U : 0U);
}

SettingReadResult<bool> read_visual_capture_notice_setting() {
  return read_bool(visual_capture_notice_value);
}

SettingWriteResult
write_visual_capture_notice_setting(const bool acknowledged) {
  return write_layerc_registry_dword(visual_capture_notice_value,
                                     acknowledged ? 1U : 0U);
}

} // namespace layerc::platform
