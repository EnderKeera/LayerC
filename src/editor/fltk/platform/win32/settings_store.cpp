#include "settings_store.hpp"

#include "appearance_contrast.hpp"

#include "layerc/platform/platform_events.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"

namespace layerc::editor {
namespace {

constexpr auto appearance_load_error =
    "The editor appearance settings could not be read; defaults are in use.";

static_assert(editor_role_count == static_cast<std::size_t>(platform::EditorControlRole::count));

AppearanceLoad decode_appearance(
    const platform::SettingReadResult<platform::EditorAppearanceSettings> &read) {
  if (read.failed()) return {{}, appearance_load_error};
  return {to_editor_appearance(read.value.value_or(platform::EditorAppearanceSettings{})), {}};
}

} // namespace

EditorAppearance to_editor_appearance(const platform::EditorAppearanceSettings &settings) {
  EditorAppearance appearance;
  appearance.theme = static_cast<EditorTheme>(settings.theme);
  if (settings.accent != 0) appearance.accent = settings.accent;
  for (std::size_t i = 0; i < appearance.overrides.size(); ++i) {
    const auto &source = settings.overrides[i];
    auto &target = appearance.overrides[i];
    if (source.fields & platform::editor_fill) target.fill = source.fill;
    if (source.fields & platform::editor_text) target.text = source.text;
    if (source.fields & platform::editor_selection) target.selection = source.selection;
  }
  return appearance;
}

platform::EditorAppearanceSettings
merge_editor_appearance(const EditorAppearance &appearance,
                        platform::EditorAppearanceSettings current) {
  current.theme = static_cast<platform::EditorTheme>(appearance.theme);
  // The registry reserves 0 for the preset; store explicit black as near-black.
  current.accent = appearance.accent.value_or(0);
  if (appearance.accent == 0U) current.accent = platform::editor_explicit_black_accent;
  for (std::size_t i = 0; i < appearance.overrides.size(); ++i) {
    const auto &source = appearance.overrides[i];
    auto &target = current.overrides[i];
    target.fields &= ~(platform::editor_fill | platform::editor_text | platform::editor_selection);
    if (source.fill) target.fields |= platform::editor_fill;
    if (source.text) target.fields |= platform::editor_text;
    if (source.selection) target.fields |= platform::editor_selection;
    target.fill = source.fill.value_or(0);
    target.text = source.text.value_or(0);
    target.selection = source.selection.value_or(0);
  }
  return current;
}

StartupAppearance load_startup_appearance() {
  if (!platform::migrate_editor_appearance())
    return {{}, {{}, appearance_load_error}};
  const auto read = platform::read_editor_appearance();
  return {read.value.value_or(platform::EditorAppearanceSettings{}), decode_appearance(read)};
}

AppearanceLoad load_platform_appearance() {
  return decode_appearance(platform::read_editor_appearance());
}

std::optional<AppearanceSaveFailure>
save_platform_appearance(const EditorAppearance &appearance, const bool replace_unreadable) {
  const auto read = platform::read_editor_appearance();
  if (read.failed() && !replace_unreadable)
    return AppearanceSaveFailure{
        "Appearance not saved: the saved appearance settings could not be read.", true};
  const auto merged = merge_editor_appearance(
      appearance, read.value.value_or(platform::EditorAppearanceSettings{}));
  if (!platform::native_high_contrast())
    if (const auto failure = first_contrast_failure(merged))
      return AppearanceSaveFailure{
          "Appearance not saved: " + platform::wide_to_utf8(contrast_failure_text(*failure))};
  if (!platform::write_editor_appearance(merged))
    return AppearanceSaveFailure{"Appearance not saved: Windows refused the setting."};
  return std::nullopt;
}

const char *appearance_replacement_prompt() noexcept {
  return "Applying a new choice replaces the saved settings.";
}

std::optional<RoleContrastFailure> shared_appearance_failure(const EditorAppearance &appearance) {
  const auto read = platform::read_editor_appearance();
  if (read.failed()) return std::nullopt;
  const auto merged = merge_editor_appearance(
      appearance, read.value.value_or(platform::EditorAppearanceSettings{}));
  // The Win32 editor's own check over the record it paints.
  const auto failure = first_contrast_failure(merged);
  if (!failure) return std::nullopt;
  const auto index = static_cast<std::size_t>(failure->role);
  ContrastPairKind pair = ContrastPairKind::text_fill;
  switch (failure->pair) {
  case ContrastPair::disabled_fill: pair = ContrastPairKind::disabled_fill; break;
  case ContrastPair::text_hover: pair = ContrastPairKind::text_hover; break;
  case ContrastPair::text_selection: pair = ContrastPairKind::text_selection; break;
  case ContrastPair::text_fill: break;
  }
  return RoleContrastFailure{static_cast<EditorRole>(index), pair, failure->ratio, {},
                             SurfacePart::fill, failure->primary_button};
}

int load_editor_text_scale() {
  return normalize_editor_text_scale(platform::read_editor_text_scale().value.value_or(0));
}

bool editor_options_supported() noexcept { return true; }

EditorOptionsLoad load_editor_options(const bool capture_exclusion, const int text_scale) {
  using platform::BoolSetting;
  const auto visual = platform::read_bool_setting(BoolSetting::visual_recognition);
  return {
      {
          .visual_recognition = visual.value.value_or(visual.missing()),
          .overlay_capture_exclusion =
              platform::read_bool_setting(BoolSetting::capture_exclusion).value.value_or(true),
          .editor_capture_exclusion = capture_exclusion,
          .launcher_updates =
              platform::read_bool_setting(BoolSetting::launcher_updates).value.value_or(false),
          .text_scale = text_scale,
      },
      visual.failed() ? "Could not read the visual-recognition setting." : "",
  };
}

std::optional<std::string_view>
options_commit_error(const platform::SettingsCommitState state) noexcept {
  using platform::SettingsCommitState;
  if (state == SettingsCommitState::committed) return std::nullopt;
  if (state == SettingsCommitState::partial)
    return "Could not save the selected options. Windows refused a "
           "setting, and some options it had already saved could not be "
           "restored. Choose Apply again to save them all.";
  return "Could not save the selected options. Windows refused a "
         "setting, so no option was changed. Choose Apply to try again.";
}

void notify_editor_options_reload() { (void)platform::signal_settings_reload(); }

std::optional<std::string_view>
commit_editor_options(const EditorOptions &options, void (*const reload)()) {
  const auto written = platform::write_options_settings({
      .capture_exclusion = options.overlay_capture_exclusion,
      .editor_capture_exclusion = options.editor_capture_exclusion,
      .visual_recognition = options.visual_recognition,
      .launcher_updates = options.launcher_updates,
      .editor_text_scale = static_cast<std::uint32_t>(options.text_scale),
  });
  if (const auto error = options_commit_error(written.state)) return error;
  reload();
  return std::nullopt;
}

} // namespace layerc::editor
