#pragma once

#include "layerc/platform/editor_appearance.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::editor {

// The minimums Appearance enforces (DesignSystem.md, Roles). Apply and OK
// refuse below them; the role list's warning line and Choose color's
// contrast line show them while the colors change, in the same words.
enum class ContrastPair { text_fill, disabled_fill, text_hover, text_selection };

struct ContrastFailure {
  layerc::platform::EditorControlRole role{};
  ContrastPair pair{};
  double ratio{};
};

[[nodiscard]] double contrast_minimum(ContrastPair pair) noexcept;
[[nodiscard]] std::wstring_view contrast_pair_name(ContrastPair pair) noexcept;
// Hover contrast applies to interactive chrome only; selection contrast also
// to the lists.
[[nodiscard]] bool contrast_pair_applies(layerc::platform::EditorControlRole role,
                                         ContrastPair pair) noexcept;
// The override field to adjust when `pair` fails.
[[nodiscard]] std::uint32_t contrast_pair_field(ContrastPair pair) noexcept;
// The first role, in list order, whose colors fall below a minimum. The
// overrides are measured against the preset they paint over, even while a
// contrast theme sets them aside.
[[nodiscard]] std::optional<ContrastFailure>
first_contrast_failure(const layerc::platform::EditorAppearanceSettings &settings);

// The control-role list's row text ("Saved profiles") and the singular noun
// a contrast line names ("Profile list text").
[[nodiscard]] std::wstring_view
appearance_role_label(layerc::platform::EditorControlRole role) noexcept;
[[nodiscard]] std::wstring_view
appearance_role_noun(layerc::platform::EditorControlRole role) noexcept;

// "12.9:1", rounded down so a ratio just below a minimum never reads as it.
[[nodiscard]] std::wstring contrast_ratio_text(double ratio);
// "Buttons: Text / Hover contrast is 3.2:1; at least 4.5:1 is required."
[[nodiscard]] std::wstring contrast_failure_text(const ContrastFailure &failure);

// A color a candidate is measured against in Choose color. `required` pairs
// are ones Appearance refuses below; others only warn.
struct ContrastTarget {
  std::wstring name;
  COLORREF color{};
  double minimum{};
  bool required{true};
};

struct ContrastLine {
  std::wstring text;
  bool below_minimum{};
};

// What a new color for `field` of `role` is measured against: the colors it
// pairs with in that role, as they would paint with the staged settings.
[[nodiscard]] std::vector<ContrastTarget>
appearance_contrast_targets(const layerc::platform::EditorAppearanceSettings &settings,
                            layerc::platform::EditorControlRole role,
                            std::uint32_t field);
// The pair nearest its minimum (or furthest below it): "Contrast with Window
// text: 12.9:1", or below the minimum "Contrast with Window text: 3.2:1;
// Appearance needs 4.5:1." ("...; aim for 3:1." when not required). Empty
// targets give no line.
[[nodiscard]] std::optional<ContrastLine>
picker_contrast_line(COLORREF candidate, const std::vector<ContrastTarget> &targets);
// The screen-reader description of every Choose color swatch, in palette
// order: each swatch's own color measured against `targets` with the line
// above, so a swatch never describes the selected color (DesignSystem.md,
// Screen readers). Empty strings when there are no targets.
[[nodiscard]] std::vector<std::wstring>
palette_swatch_descriptions(const std::vector<ContrastTarget> &targets);

// An overlay bar's own colors, measured against the Overlay rules
// (DesignSystem.md, OverlayBar): text 4.5:1 on Fill and Background while the
// bar shows it, the border 3:1 on Background.
struct OverlayColors {
  COLORREF fill{};
  COLORREF background{};
  COLORREF border{};
  COLORREF text{};
  bool show_text{true};
};

enum class OverlayColorField : std::uint8_t { fill, background, border, text };

// "Low contrast. Text is 1.7:1 on Fill and 1.9:1 on Background; aim for
// 4.5:1. Border is 1.4:1 on Background; aim for 3:1." Empty once every pair
// passes. It only warns: the profile keeps its colors.
[[nodiscard]] std::wstring overlay_contrast_text(const OverlayColors &colors);
// What Choose color measures a new `field` color against; never required.
[[nodiscard]] std::vector<ContrastTarget>
overlay_contrast_targets(const OverlayColors &colors, OverlayColorField field);

} // namespace layerc::editor
