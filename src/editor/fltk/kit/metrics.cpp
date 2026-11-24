#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <utility>

namespace layerc::editor::kit {
namespace {
constexpr int base_body = 12;

inline constexpr int win_control_height = 26;
inline constexpr int win_combo_height = 23;
inline constexpr int win_row_pitch = 34;
inline constexpr int win_list_row = 27;
inline constexpr int win_profile_row = 21;
inline constexpr int win_heading_pitch = 28;
inline constexpr int win_chevron_zone = 26;
inline constexpr int win_status_strip = 28;
inline constexpr int win_sidebar_min = 200;
inline constexpr int win_sidebar_max = 280;
inline constexpr int win_preview_from = 1100;
inline constexpr int win_slot_min = 260;
inline constexpr int win_field_cap = 480;
inline constexpr int win_timing_label = 84;
inline constexpr int win_gap_dialog = 8;
inline constexpr int win_margin = 12;
inline constexpr int win_gap_main = 16;
inline constexpr int win_section_gap = 12;
inline constexpr int outline = 1;
inline constexpr int focus_width = 2;
inline constexpr int accent_rail = 3;
inline constexpr int checkbox = 16;
inline constexpr int swatch = 16;
inline constexpr int swatch_inset = 8;
inline constexpr int menu_item = 24;
inline constexpr int scrollbar_track = 12;
inline constexpr int scrollbar_thumb = 6;
inline constexpr int tooltip_max = 360;
inline constexpr int key_chip = 20;
inline constexpr int key_chip_padding = 6;
inline constexpr int button_min_width = 88;
inline constexpr int button_text_pad = 24;
inline constexpr int status_glyph = 12;

int base_value(const Metric metric) noexcept {
  switch (metric) {
  case Metric::control_height: return win_control_height;
  case Metric::combo_height: return win_combo_height;
  case Metric::row_pitch: return win_row_pitch;
  case Metric::list_row: return win_list_row;
  case Metric::profile_row: return win_profile_row;
  case Metric::heading_pitch: return win_heading_pitch;
  case Metric::chevron_zone: return win_chevron_zone;
  case Metric::status_strip: return win_status_strip;
  case Metric::sidebar_min: return win_sidebar_min;
  case Metric::sidebar_max: return win_sidebar_max;
  case Metric::preview_from: return win_preview_from;
  case Metric::slot_min: return win_slot_min;
  case Metric::field_cap: return win_field_cap;
  case Metric::timing_label: return win_timing_label;
  case Metric::gap_dialog: return win_gap_dialog;
  case Metric::margin: return win_margin;
  case Metric::gap_main: return win_gap_main;
  case Metric::section_gap: return win_section_gap;
  case Metric::outline: return outline;
  case Metric::focus_width: return focus_width;
  case Metric::accent_rail: return accent_rail;
  case Metric::checkbox: return checkbox;
  case Metric::swatch: return swatch;
  case Metric::swatch_inset: return swatch_inset;
  case Metric::menu_item: return menu_item;
  case Metric::scrollbar_track: return scrollbar_track;
  case Metric::scrollbar_thumb: return scrollbar_thumb;
  case Metric::tooltip_max: return tooltip_max;
  case Metric::key_chip: return key_chip;
  case Metric::key_chip_padding: return key_chip_padding;
  case Metric::button_min_width: return button_min_width;
  case Metric::button_text_pad: return button_text_pad;
  case Metric::status_glyph: return status_glyph;
  }
  return 0;
}

int current_body = base_body;
MetricsSubscription last_token{};
std::map<MetricsSubscription, std::shared_ptr<std::function<void()>>> subscribers;

} // namespace

int metric_at(const Metric metric, const int body) noexcept {
  const double scaled = static_cast<double>(base_value(metric)) * std::max(body, base_body) /
                        base_body;
  return static_cast<int>(std::lround(std::min(scaled, static_cast<double>(std::numeric_limits<int>::max()))));
}

int body() noexcept { return current_body; }

int metric(const Metric value) noexcept { return metric_at(value, body()); }

void set_body(const int body) {
  if (body == current_body) return;
  current_body = body;
  const auto last = last_token;
  for (auto it = subscribers.begin(); it != subscribers.end() && it->first <= last;) {
    const auto token = it->first;
    const auto callback = it->second;
    (*callback)();
    it = subscribers.upper_bound(token);
  }
}

MetricsSubscription subscribe_metrics_changed(std::function<void()> callback) {
  const auto token = ++last_token;
  subscribers.emplace(token, std::make_shared<std::function<void()>>(std::move(callback)));
  return token;
}

void unsubscribe_metrics_changed(const MetricsSubscription token) {
  subscribers.erase(token);
}

} // namespace layerc::editor::kit
