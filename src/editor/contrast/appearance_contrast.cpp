#include "appearance_contrast.hpp"

#include "appearance_floor.hpp"

#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"
#include "overlay_contrast.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <utility>

namespace layerc::editor {
namespace {

using layerc::platform::EditorControlRole;

constexpr std::array contrast_pairs{
    ContrastPair::text_fill, ContrastPair::disabled_fill,
    ContrastPair::text_hover, ContrastPair::text_selection};

struct RoleNames {
  std::wstring_view label;
  std::wstring_view noun;
};

constexpr std::array<RoleNames, static_cast<std::size_t>(EditorControlRole::count)>
    role_names{{
        {L"Window", L"Window"},
        {L"Toolbar", L"Toolbar"},
        {L"Saved profiles", L"Profile list"},
        {L"Labels", L"Label"},
        {L"Buttons", L"Button"},
        {L"Text inputs", L"Text input"},
        {L"Lists", L"List"},
        {L"Check boxes", L"Check box"},
        {L"Preview", L"Preview"},
        {L"Status", L"Status"},
    }};

layerc::platform::NativeThemeColors
role_colors(const layerc::platform::EditorAppearanceSettings &settings,
            const EditorControlRole role) {
  return layerc::platform::preset_theme_colors(
      settings.theme, settings.accent, role,
      settings.overrides[static_cast<std::size_t>(role)]);
}

// The two colors a pair measures in one role's resolved colors.
std::pair<COLORREF, COLORREF>
pair_colors(const layerc::platform::NativeThemeColors &colors,
            const ContrastPair pair) {
  switch (pair) {
  case ContrastPair::disabled_fill: return {colors.disabled, colors.fill};
  case ContrastPair::text_hover: return {colors.text, colors.hover};
  case ContrastPair::text_selection: return {colors.text, colors.selection};
  case ContrastPair::text_fill: break;
  }
  return {colors.text, colors.fill};
}

// The shared floors and wording (src/editor/common/appearance_floor) under
// the Win32 pair names, one to one.
ContrastPairKind pair_kind(const ContrastPair pair) noexcept {
  switch (pair) {
  case ContrastPair::disabled_fill: return ContrastPairKind::disabled_fill;
  case ContrastPair::text_hover: return ContrastPairKind::text_hover;
  case ContrastPair::text_selection: return ContrastPairKind::text_selection;
  case ContrastPair::text_fill: break;
  }
  return ContrastPairKind::text_fill;
}

// A System pair is changed only if its foreground or actual background is
// authored. A retained border or hover override does not change Text / Fill.
bool system_pair_changed(const std::uint32_t fields, const ContrastPair pair,
                         const bool accent_overridden) {
  using namespace layerc::platform;
  switch (pair) {
  case ContrastPair::disabled_fill: return (fields & (editor_disabled | editor_fill)) != 0;
  case ContrastPair::text_hover: return (fields & (editor_text | editor_hover)) != 0;
  case ContrastPair::text_selection:
    return accent_overridden || (fields & (editor_text | editor_selection)) != 0;
  case ContrastPair::text_fill: return (fields & (editor_text | editor_fill)) != 0;
  }
  return false;
}

std::wstring minimum_text(const double minimum) {
  std::array<wchar_t, 16> buffer{};
  if (minimum == std::floor(minimum))
    std::swprintf(buffer.data(), buffer.size(), L"%d", static_cast<int>(minimum));
  else
    std::swprintf(buffer.data(), buffer.size(), L"%.1f", minimum);
  return buffer.data();
}

std::wstring target_name(const EditorControlRole role,
                         const std::wstring_view part) {
  return std::wstring(appearance_role_noun(role)) + L" " + std::wstring(part);
}

} // namespace

double contrast_minimum(const ContrastPair pair) noexcept {
  return contrast_floor(pair_kind(pair));
}

std::wstring_view contrast_pair_name(const ContrastPair pair) noexcept {
  switch (pair) {
  case ContrastPair::disabled_fill: return L"Disabled / Fill";
  case ContrastPair::text_hover: return L"Text / Hover";
  case ContrastPair::text_selection: return L"Text / Selection";
  case ContrastPair::text_fill: break;
  }
  return L"Text / Fill";
}

bool contrast_pair_applies(const EditorControlRole role,
                           const ContrastPair pair) noexcept {
  if (pair == ContrastPair::text_hover)
    return role == EditorControlRole::button || role == EditorControlRole::checkbox;
  if (pair == ContrastPair::text_selection)
    return role == EditorControlRole::button || role == EditorControlRole::checkbox ||
           role == EditorControlRole::list || role == EditorControlRole::sidebar;
  return true;
}

std::uint32_t contrast_pair_field(const ContrastPair pair) noexcept {
  switch (pair) {
  case ContrastPair::disabled_fill: return layerc::platform::editor_disabled;
  case ContrastPair::text_hover: return layerc::platform::editor_hover;
  case ContrastPair::text_selection: return layerc::platform::editor_selection;
  case ContrastPair::text_fill: break;
  }
  return layerc::platform::editor_text;
}

std::optional<ContrastFailure>
first_contrast_failure(const layerc::platform::EditorAppearanceSettings &settings) {
  using namespace layerc::platform;
  const bool system = settings.theme == EditorTheme::system;
  const bool accent_overridden = settings.accent != 0;
  for (std::size_t index = 0; index < settings.overrides.size(); ++index) {
    const auto role = static_cast<EditorControlRole>(index);
    const auto colors = role_colors(settings, role);
    for (const auto pair : contrast_pairs) {
      if (!contrast_pair_applies(role, pair) ||
          (system && !system_pair_changed(settings.overrides[index].fields, pair, accent_overridden)))
        continue;
      const auto [foreground, background] = pair_colors(colors, pair);
      const double ratio = layerc::platform::contrast_ratio(foreground, background);
      if (ratio < contrast_minimum(pair))
        return ContrastFailure{role, pair, ratio};
    }
  }

  constexpr auto role = EditorControlRole::button;
  const auto colors = role_colors(settings, role);
  const auto fields = settings.overrides[static_cast<std::size_t>(role)].fields;
  const auto check_primary = [&](const ContrastPair pair, const bool pressed, const bool hot)
      -> std::optional<ContrastFailure> {
    const double ratio = contrast_ratio(colors.on_accent, native_primary_button_fill(colors, pressed, hot));
    if (ratio < contrast_minimum(pair))
      return ContrastFailure{role, pair, ratio, true};
    return std::nullopt;
  };
  if (!system || accent_overridden)
    if (auto failure = check_primary(ContrastPair::text_fill, false, false)) return failure;
  if (!system || accent_overridden || (fields & editor_text) != 0)
    if (auto failure = check_primary(ContrastPair::text_hover, false, true)) return failure;
  if (!system || accent_overridden || (fields & editor_fill) != 0)
    return check_primary(ContrastPair::text_selection, true, false);
  return std::nullopt;
}

std::wstring_view appearance_role_label(const EditorControlRole role) noexcept {
  const auto index = static_cast<std::size_t>(role);
  return index < role_names.size() ? role_names[index].label : role_names[0].label;
}

std::wstring_view appearance_role_noun(const EditorControlRole role) noexcept {
  const auto index = static_cast<std::size_t>(role);
  return index < role_names.size() ? role_names[index].noun : role_names[0].noun;
}

std::wstring contrast_ratio_text(const double ratio) {
  return layerc::platform::utf8_to_wide(contrast_ratio_label(ratio));
}

std::wstring contrast_failure_text(const ContrastFailure &failure) {
  return layerc::platform::utf8_to_wide(contrast_refusal_text(
      layerc::platform::wide_to_utf8(appearance_role_label(failure.role)),
      pair_kind(failure.pair), failure.ratio,
      failure.primary_button ? primary_button_surface(pair_kind(failure.pair)) : std::string_view{}));
}

std::vector<ContrastTarget>
appearance_contrast_targets(const layerc::platform::EditorAppearanceSettings &settings,
                            const EditorControlRole role, const std::uint32_t field) {
  const auto colors = role_colors(settings, role);
  const auto applies = [role](const ContrastPair pair) {
    return contrast_pair_applies(role, pair);
  };
  std::vector<ContrastTarget> targets;
  switch (field) {
  case layerc::platform::editor_fill:
    targets.push_back({target_name(role, L"text"), colors.text, 4.5});
    targets.push_back({target_name(role, L"disabled text"), colors.disabled, 3.0});
    break;
  case layerc::platform::editor_text:
    targets.push_back({target_name(role, L"fill"), colors.fill, 4.5});
    if (applies(ContrastPair::text_hover))
      targets.push_back({target_name(role, L"hover"), colors.hover, 4.5});
    if (applies(ContrastPair::text_selection))
      targets.push_back({target_name(role, L"selection"), colors.selection, 4.5});
    break;
  case layerc::platform::editor_hover:
    if (applies(ContrastPair::text_hover))
      targets.push_back({target_name(role, L"text"), colors.text, 4.5});
    break;
  case layerc::platform::editor_selection:
    if (applies(ContrastPair::text_selection))
      targets.push_back({target_name(role, L"text"), colors.text, 4.5});
    break;
  case layerc::platform::editor_disabled:
    targets.push_back({target_name(role, L"fill"), colors.fill, 3.0});
    break;
  case layerc::platform::editor_border:
    // Outlines warn below 3:1 against their fill but never refuse.
    targets.push_back({target_name(role, L"fill"), colors.fill, 3.0, false});
    break;
  default:
    break;
  }
  return targets;
}

std::optional<ContrastLine>
picker_contrast_line(const COLORREF candidate,
                     const std::vector<ContrastTarget> &targets) {
  const ContrastTarget *worst{};
  double worst_ratio{};
  double worst_margin{};
  for (const auto &target : targets) {
    const double ratio = layerc::platform::contrast_ratio(candidate, target.color);
    const double margin = ratio / target.minimum;
    if (worst == nullptr || margin < worst_margin) {
      worst = &target;
      worst_ratio = ratio;
      worst_margin = margin;
    }
  }
  if (worst == nullptr)
    return std::nullopt;
  ContrastLine line;
  line.below_minimum = worst_ratio < worst->minimum;
  line.text = L"Contrast with " + worst->name + L": " + contrast_ratio_text(worst_ratio);
  if (line.below_minimum)
    line.text += worst->required
                     ? L"; Appearance needs " + minimum_text(worst->minimum) + L":1."
                     : L"; aim for " + minimum_text(worst->minimum) + L":1.";
  return line;
}

std::vector<std::wstring>
palette_swatch_descriptions(const std::vector<ContrastTarget> &targets) {
  std::vector<std::wstring> descriptions;
  descriptions.reserve(layerc::platform::color_palette.size());
  for (const auto &swatch : layerc::platform::color_palette) {
    auto line = picker_contrast_line(
        layerc::platform::native_color_ref(swatch.value), targets);
    descriptions.push_back(line ? std::move(line->text) : std::wstring{});
  }
  return descriptions;
}

// One wording source for both editors (src/editor/common/overlay_contrast).
std::wstring overlay_contrast_text(const OverlayColors &colors) {
  using layerc::platform::native_rgb_value;
  return layerc::platform::utf8_to_wide(overlay_contrast_line(
      native_rgb_value(colors.fill), native_rgb_value(colors.background),
      native_rgb_value(colors.border), native_rgb_value(colors.text),
      colors.show_text));
}

std::vector<ContrastTarget>
overlay_contrast_targets(const OverlayColors &colors,
                         const OverlayColorField field) {
  std::vector<ContrastTarget> targets;
  switch (field) {
  case OverlayColorField::text:
    targets.push_back({L"Fill", colors.fill, overlay_text_minimum, false});
    targets.push_back(
        {L"Background", colors.background, overlay_text_minimum, false});
    break;
  case OverlayColorField::background:
    targets.push_back(
        {L"Border", colors.border, overlay_mark_minimum, false});
    [[fallthrough]];
  case OverlayColorField::fill:
    if (colors.show_text)
      targets.push_back({L"Text", colors.text, overlay_text_minimum, false});
    break;
  case OverlayColorField::border:
    targets.push_back(
        {L"Background", colors.background, overlay_mark_minimum, false});
    break;
  }
  return targets;
}

} // namespace layerc::editor
