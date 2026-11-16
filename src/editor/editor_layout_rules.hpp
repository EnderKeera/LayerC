#pragma once

#include <algorithm>

namespace layerc::editor {

// The main editor's existing minimum client width, in logical pixels. Font
// changes and interactive resizing must enforce the same scaled floor.
inline constexpr int main_client_min_width = 880;

// Stacking is usable only while its measured height fits the work area.
// Otherwise keep the three-slot form, without requiring an off-screen width.
[[nodiscard]] constexpr int main_minimum_width(
    const int narrow_width, const int narrow_height, const int wide_width,
    const int work_width, const int work_height) {
  return std::min(work_width,
                  narrow_height <= work_height ? narrow_width : wide_width);
}

// The binding field keeps the editor-shell minimum (logical 60 px). Capture key
// (and HID) stay on the binding row unless that would push the field below it,
// which is the only case where they used to overlap the binding.
inline constexpr int binding_field_floor = 60;

// All arguments are device pixels at the current scale.
[[nodiscard]] constexpr bool capture_buttons_wrap(const int timing_width,
                                                  const int label_width,
                                                  const int buttons_width,
                                                  const int gap,
                                                  const int field_floor) {
  return timing_width - label_width - buttons_width - gap < field_floor;
}

// The main window's three equal settings slots (DesignSystem.md, Layout)
// span from the detail column's left edge to the right margin, `gap` apart.
[[nodiscard]] constexpr int main_slot_width(const int client_width,
                                            const int margin,
                                            const int detail_left,
                                            const int gap) {
  return (client_width - margin - detail_left - gap * 2) / 3;
}

// Three slots and the preview corner show from `preview_from` wide while
// each slot keeps `slot_min`; otherwise the slots stack and the preview hides.
[[nodiscard]] constexpr bool main_layout_wide(const int client_width,
                                              const int slot_width,
                                              const int preview_from,
                                              const int slot_min) {
  return client_width >= preview_from && slot_width >= slot_min;
}

// The settings row is anchored to `bottom` and the list takes the rest, with
// no cap; the list keeps `min_list`, below which the row is pushed down and
// the window grows instead.
[[nodiscard]] constexpr int main_groups_top(const int list_top,
                                            const int min_list, const int gap,
                                            const int bottom,
                                            const int groups_height) {
  const int anchored = bottom - groups_height;
  const int lowest = list_top + min_list + gap;
  return anchored > lowest ? anchored : lowest;
}

} // namespace layerc::editor
