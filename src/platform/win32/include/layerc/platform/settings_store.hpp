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

enum class BoolSetting : std::uint8_t {
  // Cooldown overlays (LayerD). Missing means excluded.
  capture_exclusion,
  visual_recognition,
  visual_capture_notice,
  // LayerC Editor and its dialogs. Missing means capturable, so screenshots,
  // screen sharing and Magnifier see the editor unless the user opts out.
  editor_capture_exclusion,
  // The editor's "Show advanced" view toggle.
  editor_advanced_view,
  // The launcher's (layerc.exe) update check, the same Updates DWORD it reads.
  // Missing means off: the launcher makes no network requests.
  launcher_updates,
};

[[nodiscard]] SettingReadResult<bool> read_bool_setting(BoolSetting setting);
[[nodiscard]] SettingWriteResult write_bool_setting(BoolSetting setting,
                                                    bool value);

// Registry primitives below share the single HKCU\Software\LayerC root (or the
// HKCU-relative key in LAYERC_SETTINGS_KEY, which benchmarks and tests use to
// keep the user's settings untouched). They are exposed for platform-owned
// structured settings such as appearance and HID stable slots; product code
// should use the typed setting functions above.
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
