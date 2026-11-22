#include "platform/win32/contrast_palette.hpp"

namespace layerc::editor {

ContrastPalette make_windows_contrast_palette(
    const std::array<platform::NativeThemeColors, editor_role_count> &roles) noexcept {
  using platform::native_rgb_value;
  constexpr auto index = [](EditorRole role) { return static_cast<std::size_t>(role); };
  ContrastPalette palette{};
  for (std::size_t i = 0; i < roles.size(); ++i) {
    const auto &role = roles[i];
    palette.roles[i] = {native_rgb_value(role.fill), native_rgb_value(role.text),
                        native_rgb_value(role.selection)};
  }
  const auto &window = roles[index(EditorRole::window)];
  const auto &button = roles[index(EditorRole::button)];
  const auto &list = roles[index(EditorRole::list)];
  palette.tokens = {
      .background = native_rgb_value(window.fill),
      .panel = native_rgb_value(button.fill),
      .input = native_rgb_value(window.input),
      .border = native_rgb_value(window.border),
      .divider = native_rgb_value(window.divider),
      .text = native_rgb_value(window.text),
      .secondary = native_rgb_value(window.secondary),
      .subtext = native_rgb_value(window.subtext),
      .verbose = native_rgb_value(window.verbose),
      .disabled = native_rgb_value(window.disabled),
      .button_hover = native_rgb_value(button.hover),
      .row_hover = native_rgb_value(list.hover),
      .pressed = native_rgb_value(button.pressed),
      .accent = native_rgb_value(window.accent),
      .selection = native_rgb_value(window.selection),
      .on_accent = native_rgb_value(window.on_accent),
      .yes = native_rgb_value(window.yes),
      .no = native_rgb_value(window.no),
      .warning = native_rgb_value(window.warning),
      .accent_2 = native_rgb_value(window.accent_2),
      .on_status = native_rgb_value(window.on_status),
  };
  return palette;
}

std::optional<ContrastPalette> platform_contrast_palette() {
  if (!platform::native_high_contrast()) return std::nullopt;
  std::array<platform::NativeThemeColors, editor_role_count> roles;
  for (std::size_t i = 0; i < roles.size(); ++i)
    roles[i] = platform::high_contrast_colors(static_cast<platform::EditorControlRole>(i));
  return make_windows_contrast_palette(roles);
}

} // namespace layerc::editor
