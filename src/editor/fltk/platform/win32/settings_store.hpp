#pragma once

#include "platform/editor_appearance_store.hpp"
#include "platform/editor_options.hpp"

#include "layerc/platform/editor_appearance.hpp"
#include "layerc/platform/settings_store.hpp"

namespace layerc::editor {

struct StartupAppearance {
  platform::EditorAppearanceSettings native;
  AppearanceLoad editor;
};

[[nodiscard]] EditorAppearance
to_editor_appearance(const platform::EditorAppearanceSettings &settings);
[[nodiscard]] platform::EditorAppearanceSettings
merge_editor_appearance(const EditorAppearance &appearance,
                        platform::EditorAppearanceSettings current);
[[nodiscard]] StartupAppearance load_startup_appearance();

[[nodiscard]] int load_editor_text_scale();
[[nodiscard]] std::optional<std::string_view>
options_commit_error(platform::SettingsCommitState state) noexcept;

} // namespace layerc::editor
