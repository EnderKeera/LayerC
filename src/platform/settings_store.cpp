#include "layerc/platform/settings_store.hpp"

#include <cstring>
#include <limits>
#include <utility>

#include <windows.h>

namespace layerc::platform {
namespace {

constexpr wchar_t settings_registry_key[] = L"Software\\LayerC";
constexpr wchar_t capture_exclusion_value[] = L"ExcludeFromCapture";
constexpr wchar_t visual_recognition_value[] = L"VisualRecognitionEnabled";
constexpr wchar_t visual_capture_notice_value[] = L"VisualCaptureNoticeSeen";

class UniqueHKey {
public:
  explicit UniqueHKey(HKEY key = nullptr) noexcept : key_(key) {}
  ~UniqueHKey() { if (key_ != nullptr) RegCloseKey(key_); }
  UniqueHKey(const UniqueHKey &) = delete;
  UniqueHKey &operator=(const UniqueHKey &) = delete;
  [[nodiscard]] HKEY get() const noexcept { return key_; }
private:
  HKEY key_;
};

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

SettingWriteResult set_layerc_value(const std::wstring_view subkey,
                                   const std::wstring_view value_name,
                                   const DWORD type, const void *const data,
                                   const DWORD bytes) {
  const auto opened = open_layerc_key(subkey, KEY_SET_VALUE, true);
  if (opened.key == nullptr)
    return {.error = static_cast<std::uint32_t>(opened.status)};
  const UniqueHKey key(opened.key);
  const std::wstring name(value_name);
  const LSTATUS status = RegSetValueExW(
      key.get(), name.c_str(), 0, type, static_cast<const BYTE *>(data), bytes);
  return {.error = static_cast<std::uint32_t>(status)};
}

template <typename T>
SettingReadResult<T> read_error(const LSTATUS status) {
  return {.value = std::nullopt,
          .error = is_missing(status) ? 0U
                                      : static_cast<std::uint32_t>(status)};
}

SettingReadResult<std::vector<std::byte>> query_value(
    const std::wstring_view value_name, const std::wstring_view subkey,
    const DWORD type, const std::size_t maximum_bytes) {
  if (maximum_bytes > std::numeric_limits<DWORD>::max())
    return {.value = std::nullopt, .error = ERROR_FILE_TOO_LARGE};
  const auto opened = open_layerc_key(subkey, KEY_QUERY_VALUE, false);
  if (opened.key == nullptr)
    return read_error<std::vector<std::byte>>(opened.status);
  const UniqueHKey key(opened.key);
  const std::wstring name(value_name);
  std::vector<std::byte> value(maximum_bytes);
  DWORD size = static_cast<DWORD>(maximum_bytes);
  const LSTATUS status = RegGetValueW(key.get(), nullptr, name.c_str(), type,
                                      nullptr, value.data(), &size);
  if (status == ERROR_MORE_DATA)
    return {.value = std::nullopt, .error = ERROR_FILE_TOO_LARGE};
  if (status != ERROR_SUCCESS)
    return read_error<std::vector<std::byte>>(status);
  if (size > maximum_bytes)
    return {.value = std::nullopt, .error = ERROR_FILE_TOO_LARGE};
  value.resize(size);
  return {.value = std::move(value), .error = 0};
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
  const auto bytes =
      query_value(value_name, subkey, RRF_RT_REG_DWORD, sizeof(std::uint32_t));
  if (!bytes.value)
    return {.value = std::nullopt, .error = bytes.error};
  if (bytes.value->size() != sizeof(std::uint32_t))
    return {.value = std::nullopt, .error = ERROR_INVALID_DATA};
  std::uint32_t value{};
  std::memcpy(&value, bytes.value->data(), sizeof(value));
  return {.value = value, .error = 0};
}

SettingWriteResult
write_layerc_registry_dword(const std::wstring_view value_name,
                            const std::uint32_t value,
                            const std::wstring_view subkey) {
  return set_layerc_value(subkey, value_name, REG_DWORD, &value, sizeof(value));
}

SettingReadResult<std::vector<std::byte>>
read_layerc_registry_binary(const std::wstring_view value_name,
                            const std::size_t maximum_bytes,
                            const std::wstring_view subkey) {
  return query_value(value_name, subkey, RRF_RT_REG_BINARY, maximum_bytes);
}

SettingWriteResult
write_layerc_registry_binary(const std::wstring_view value_name,
                             const std::span<const std::byte> value,
                             const std::wstring_view subkey) {
  if (value.size() > std::numeric_limits<DWORD>::max())
    return {.error = ERROR_FILE_TOO_LARGE};
  return set_layerc_value(subkey, value_name, REG_BINARY, value.data(),
                          static_cast<DWORD>(value.size()));
}

SettingReadResult<std::wstring>
read_layerc_registry_string(const std::wstring_view value_name,
                            const std::size_t maximum_characters,
                            const std::wstring_view subkey) {
  if (maximum_characters >=
      std::numeric_limits<DWORD>::max() / sizeof(wchar_t))
    return {.value = std::nullopt, .error = ERROR_FILE_TOO_LARGE};
  auto result = query_value(value_name, subkey, RRF_RT_REG_SZ,
                            (maximum_characters + 1U) * sizeof(wchar_t));
  if (!result.value) return {.value = std::nullopt, .error = result.error};
  const auto &bytes = *result.value;
  if (bytes.size() % sizeof(wchar_t) != 0)
    return {.value = std::nullopt, .error = ERROR_INVALID_DATA};
  std::wstring value(bytes.size() / sizeof(wchar_t), L'\0');
  if (!bytes.empty())
    std::memcpy(value.data(), bytes.data(), bytes.size());
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
  // REG_SZ data includes the terminator, which a string_view may not carry.
  const std::wstring stored(value);
  return set_layerc_value(subkey, value_name, REG_SZ, stored.c_str(),
                          static_cast<DWORD>(characters * sizeof(wchar_t)));
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
