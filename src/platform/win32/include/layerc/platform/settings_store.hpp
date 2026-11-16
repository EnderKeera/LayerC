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

// The editor's own text size, a DWORD percent added to the Windows setting.
[[nodiscard]] SettingReadResult<std::uint32_t> read_editor_text_scale();
[[nodiscard]] SettingWriteResult write_editor_text_scale(std::uint32_t percent);

// Settings that change as one unit: every value is written, or each value
// already written is put back exactly as it was.
enum class SettingsCommitState : std::uint8_t {
  committed, // every value was written
  unchanged, // nothing was written, or every write was undone
  partial,   // a write failed and an earlier write could not be undone
};

struct SettingsCommitResult {
  SettingsCommitState state{SettingsCommitState::committed};
  // The Win32 error that stopped the commit; 0 once committed.
  std::uint32_t error{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return state == SettingsCommitState::committed;
  }
};

// A value as it was before a commit: absent, or its exact type and bytes.
struct StoredSetting {
  std::optional<std::uint32_t> type;
  std::vector<std::byte> data;
};

// The commit policy over a store of `count` values. Every prior value is read
// before any is written, so a failed read writes nothing; values are written
// in order; after a failed write each value already written is restored,
// newest first. A failed write leaves its own value as it was. `store`
// provides, each returning 0 or a Win32 error:
//   read(index, StoredSetting &prior)
//   write(index)
//   restore(index, const StoredSetting &prior)
template <typename Store>
[[nodiscard]] SettingsCommitResult commit_settings(Store &store,
                                                   const std::size_t count) {
  std::vector<StoredSetting> prior(count);
  for (std::size_t index = 0; index < count; ++index)
    if (const std::uint32_t error = store.read(index, prior[index]);
        error != 0)
      return {.state = SettingsCommitState::unchanged, .error = error};
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t error = store.write(index);
    if (error == 0)
      continue;
    SettingsCommitResult result{.state = SettingsCommitState::unchanged,
                                .error = error};
    for (std::size_t written = index; written-- > 0;)
      if (store.restore(written, prior[written]) != 0)
        result.state = SettingsCommitState::partial;
    return result;
  }
  return {};
}

// The choices Options > Apply persists, committed together through
// commit_settings: the four BoolSetting values it shows and the text size.
struct OptionsSettings {
  bool capture_exclusion{};
  bool editor_capture_exclusion{};
  bool visual_recognition{};
  bool launcher_updates{};
  std::uint32_t editor_text_scale{100};
};

[[nodiscard]] SettingsCommitResult
write_options_settings(const OptionsSettings &settings);

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
