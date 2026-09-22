#include "layerc/platform/editor_appearance.hpp"

#include <cstring>
#include <span>
#include <string>

#include <windows.h>

namespace layerc::platform {
namespace {

constexpr wchar_t editor_appearance_value[] = L"EditorAppearance";
constexpr wchar_t editor_theme_value[] = L"EditorTheme";
constexpr wchar_t editor_accent_value[] = L"EditorAccent";
constexpr wchar_t editor_appearance_prefix[] = L"EditorAppearance.";
constexpr std::uint32_t editor_appearance_version = 1;
constexpr std::uint32_t all_editor_appearance_fields =
    editor_fill | editor_text | editor_border | editor_hover |
    editor_selection | editor_disabled;

struct StoredEditorAppearance {
  std::uint32_t version{};
  std::uint32_t theme{};
  std::uint32_t accent{};
  std::uint32_t reserved{};
  std::array<EditorAppearanceOverrides,
             static_cast<std::size_t>(EditorControlRole::count)>
      overrides{};
};

bool is_known_editor_theme(const std::uint32_t value) {
  return value <= static_cast<std::uint32_t>(EditorTheme::dark);
}

std::wstring legacy_override_name(const EditorControlRole role) {
  return std::wstring(editor_appearance_prefix) +
         std::to_wstring(static_cast<std::uint32_t>(role));
}

SettingReadResult<EditorAppearanceSettings> read_legacy_appearance() {
  EditorAppearanceSettings settings;
  bool found{};

  const auto theme = read_layerc_registry_dword(editor_theme_value);
  if (theme.failed())
    return {.value = std::nullopt, .error = theme.error};
  if (theme.value) {
    if (*theme.value > static_cast<std::uint32_t>(EditorTheme::plum))
      return {.value = std::nullopt, .error = ERROR_INVALID_DATA};
    settings.theme = static_cast<EditorTheme>(*theme.value);
    found = true;
  }

  const auto accent = read_layerc_registry_dword(editor_accent_value);
  if (accent.failed())
    return {.value = std::nullopt, .error = accent.error};
  if (accent.value) {
    settings.accent = *accent.value & 0x00FFFFFFU;
    found = true;
  }

  for (std::size_t index = 0; index < settings.overrides.size(); ++index) {
    const auto bytes = read_layerc_registry_binary(
        legacy_override_name(static_cast<EditorControlRole>(index)),
        sizeof(EditorAppearanceOverrides));
    if (bytes.failed())
      return {.value = std::nullopt, .error = bytes.error};
    if (!bytes.value)
      continue;
    if (bytes.value->size() != sizeof(EditorAppearanceOverrides))
      return {.value = std::nullopt, .error = ERROR_INVALID_DATA};
    std::memcpy(&settings.overrides[index], bytes.value->data(),
                sizeof(EditorAppearanceOverrides));
    settings.overrides[index].fields &= all_editor_appearance_fields;
    found = true;
  }
  return {.value = found ? std::optional(settings) : std::nullopt, .error = 0};
}

} // namespace

SettingReadResult<EditorAppearanceSettings> read_editor_appearance() {
  const auto bytes = read_layerc_registry_binary(editor_appearance_value,
                                                  sizeof(StoredEditorAppearance));
  if (!bytes.value)
    return {.value = std::nullopt, .error = bytes.error};
  if (bytes.value->size() != sizeof(StoredEditorAppearance))
    return {.value = std::nullopt, .error = ERROR_INVALID_DATA};

  StoredEditorAppearance stored{};
  std::memcpy(&stored, bytes.value->data(), sizeof(stored));
  if (stored.version != editor_appearance_version ||
      !is_known_editor_theme(stored.theme))
    return {.value = std::nullopt, .error = ERROR_INVALID_DATA};

  EditorAppearanceSettings settings{
      .theme = static_cast<EditorTheme>(stored.theme),
      .accent = stored.accent & 0x00FFFFFFU,
      .overrides = stored.overrides,
  };
  for (auto &overrides : settings.overrides)
    overrides.fields &= all_editor_appearance_fields;
  return {.value = settings, .error = 0};
}

SettingWriteResult
write_editor_appearance(const EditorAppearanceSettings &settings) {
  const auto theme = static_cast<std::uint32_t>(settings.theme);
  if (!is_known_editor_theme(theme))
    return {.error = ERROR_INVALID_DATA};
  StoredEditorAppearance stored{
      .version = editor_appearance_version,
      .theme = theme,
      .accent = settings.accent & 0x00FFFFFFU,
      .overrides = settings.overrides,
  };
  for (auto &overrides : stored.overrides)
    overrides.fields &= all_editor_appearance_fields;
  return write_layerc_registry_binary(
      editor_appearance_value,
      std::as_bytes(std::span<const StoredEditorAppearance>(&stored, 1)));
}

SettingWriteResult migrate_editor_appearance() {
  const auto current = read_editor_appearance();
  if (current.value)
    return {};
  if (current.failed())
    return {.error = current.error};
  const auto legacy = read_legacy_appearance();
  if (legacy.failed())
    return {.error = legacy.error};
  return legacy.value ? write_editor_appearance(*legacy.value)
                      : SettingWriteResult{};
}

} // namespace layerc::platform
