#pragma once

namespace layerc::editor {

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

} // namespace layerc::editor
