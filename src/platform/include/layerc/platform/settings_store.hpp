#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {

template <typename T> struct SettingReadResult {
  std::optional<T> value;
  std::uint32_t error{};

  [[nodiscard]] bool missing() const noexcept {
    return !value.has_value() && error == 0;
  }
  [[nodiscard]] bool failed() const noexcept { return error != 0; }
};

struct SettingWriteResult {
  std::uint32_t error{};

  [[nodiscard]] explicit operator bool() const noexcept { return error == 0; }
};

[[nodiscard]] SettingReadResult<bool> read_capture_exclusion_setting();
[[nodiscard]] SettingWriteResult write_capture_exclusion_setting(bool enabled);
[[nodiscard]] SettingReadResult<bool> read_visual_recognition_setting();
[[nodiscard]] SettingWriteResult write_visual_recognition_setting(bool enabled);
[[nodiscard]] SettingReadResult<bool> read_visual_capture_notice_setting();
[[nodiscard]] SettingWriteResult
write_visual_capture_notice_setting(bool acknowledged);

// Registry primitives below share the single HKCU\Software\LayerC root. They
// are exposed for platform-owned structured settings such as appearance and HID
// stable slots; product code should use the typed setting functions above.
[[nodiscard]] SettingReadResult<std::uint32_t>
read_layerc_registry_dword(std::wstring_view value_name,
                           std::wstring_view subkey = {});
[[nodiscard]] SettingWriteResult
write_layerc_registry_dword(std::wstring_view value_name, std::uint32_t value,
                            std::wstring_view subkey = {});
[[nodiscard]] SettingReadResult<std::vector<std::byte>>
read_layerc_registry_binary(std::wstring_view value_name,
                            std::size_t maximum_bytes,
                            std::wstring_view subkey = {});
[[nodiscard]] SettingWriteResult
write_layerc_registry_binary(std::wstring_view value_name,
                             std::span<const std::byte> value,
                             std::wstring_view subkey = {});
[[nodiscard]] SettingReadResult<std::wstring>
read_layerc_registry_string(std::wstring_view value_name,
                            std::size_t maximum_characters,
                            std::wstring_view subkey = {});
[[nodiscard]] SettingWriteResult
write_layerc_registry_string(std::wstring_view value_name,
                             std::wstring_view value,
                             std::wstring_view subkey = {});

} // namespace layerc::platform
