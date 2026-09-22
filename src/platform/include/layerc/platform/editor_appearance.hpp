#pragma once

#include "layerc/platform/settings_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace layerc::platform {

enum class EditorTheme : std::uint32_t {
  copper = 0,
  teal = 1,
  plum = 2,
  system = 3,
  dark = 4,
};

enum class EditorControlRole : std::uint32_t {
  window = 0,
  toolbar = 1,
  sidebar = 2,
  label = 3,
  button = 4,
  edit = 5,
  list = 6,
  checkbox = 7,
  preview = 8,
  status = 9,
  count,
};

enum EditorAppearanceField : std::uint32_t {
  editor_fill = 1U << 0U,
  editor_text = 1U << 1U,
  editor_border = 1U << 2U,
  editor_hover = 1U << 3U,
  editor_selection = 1U << 4U,
  editor_disabled = 1U << 5U,
};

struct EditorAppearanceOverrides {
  std::uint32_t fields{};
  std::uint32_t fill{};
  std::uint32_t text{};
  std::uint32_t border{};
  std::uint32_t hover{};
  std::uint32_t selection{};
  std::uint32_t disabled{};
};

struct EditorAppearanceSettings {
  EditorTheme theme{EditorTheme::copper};
  std::uint32_t accent{};
  std::array<EditorAppearanceOverrides,
             static_cast<std::size_t>(EditorControlRole::count)>
      overrides{};
};

static_assert(EditorAppearanceSettings{}.overrides.size() ==
              static_cast<std::size_t>(EditorControlRole::count));

[[nodiscard]] SettingReadResult<EditorAppearanceSettings>
read_editor_appearance();
[[nodiscard]] SettingWriteResult
write_editor_appearance(const EditorAppearanceSettings &settings);
// Explicit startup migration. Pure reads never mutate the registry.
[[nodiscard]] SettingWriteResult migrate_editor_appearance();

} // namespace layerc::platform
