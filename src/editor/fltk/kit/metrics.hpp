#pragma once

#include <cstddef>
#include <functional>

namespace layerc::editor::kit {

enum class Metric {
  control_height,
  combo_height,
  row_pitch,
  list_row,
  profile_row,
  heading_pitch,
  chevron_zone,
  status_strip,
  sidebar_min,
  sidebar_max,
  preview_from,
  slot_min,
  field_cap,
  timing_label,
  gap_dialog,
  margin,
  gap_main,
  section_gap,
  outline,
  focus_width,
  accent_rail,
  checkbox,
  swatch,
  swatch_inset,
  menu_item,
  scrollbar_track,
  scrollbar_thumb,
  tooltip_max,
  key_chip,
  key_chip_padding,
  button_min_width,
  button_text_pad,
  status_glyph,
};

[[nodiscard]] int metric_at(Metric metric, int body) noexcept;
[[nodiscard]] int scaled_at(int logical, int body) noexcept;

// Current body and change subscriptions are owned by the FLTK main thread.
using MetricsSubscription = std::size_t;
[[nodiscard]] int body() noexcept;
void set_body(int body);
[[nodiscard]] int metric(Metric metric) noexcept;
[[nodiscard]] int scaled(int logical) noexcept;
[[nodiscard]] MetricsSubscription subscribe_metrics_changed(std::function<void()> callback);
void unsubscribe_metrics_changed(MetricsSubscription token);

} // namespace layerc::editor::kit
