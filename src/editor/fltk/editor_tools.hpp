#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace layerc::editor {

// The four bar colours the panel-colours dialog edits.
struct PanelColorValues {
  std::array<std::uint32_t, 4> values{};
};

// Modal panel colours (#RRGGBB fields plus a chooser per colour); nullopt when
// the user cancels. Validation and layout are unchanged.
[[nodiscard]] std::optional<PanelColorValues>
edit_panel_colors(const PanelColorValues &current);

// The Options dialog body, owned here so the command table stays data-only.
[[nodiscard]] const char *options_text();

// Where the panel-colours dialog places its #RRGGBB inputs and choosers: the
// widest "<Name> #RRGGBB" label, both columns and the resulting window width.
struct PanelColorsLayout {
  int label_width{};
  int input_x{};
  int button_x{};
  int window_width{};
};

// Measures with the inputs' default label font and size (FL_HELVETICA /
// FL_NORMAL_SIZE).
[[nodiscard]] PanelColorsLayout panel_colors_layout();

} // namespace layerc::editor
