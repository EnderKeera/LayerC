#pragma once

#include "layerc/core/companion.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace layerc::gamebar::widget {

// What one panel slot draws, derived only from the immutable snapshot. The
// widget never resolves references, evaluates variables, counts down or
// derives a flash phase: version 2 rows arrive resolved by LayerD.
struct RowModel {
  std::string label;
  // Secondary text beside the label; empty when the label already has it.
  std::string value;
  // Bar fraction in [0,1], absent for rows without progress.
  std::optional<double> fill;
  std::uint32_t pips{};
  std::uint32_t filled_pips{};
  // 0xRRGGBB accent; absent means the theme's default accent.
  std::optional<std::uint32_t> color;
  // An indicator's swatch keeps its space in both flash phases.
  bool indicator{};
  bool accent_on{};
  bool unavailable{};

  bool operator==(const RowModel &) const = default;
};

struct PanelModel {
  std::vector<RowModel> rows;
  // Empty when there is nothing to report.
  std::string status;

  bool operator==(const PanelModel &) const = default;
};

[[nodiscard]] inline std::string legacy_value_text(const core::CompanionSnapshotItem &item) {
  if (item.is_ready) return "Ready";
  std::string text = std::to_string((item.remaining_ms + 999) / 1000) + " s";
  // UTF-8 middle dot, spelled as bytes: the sources have no /utf-8 flag.
  if (item.max_charges > 1)
    text += "  \xC2\xB7  " + std::to_string(item.available_charges) + "/" +
            std::to_string(item.max_charges);
  return text;
}

[[nodiscard]] inline PanelModel
panel_model(const core::CompanionPanelSnapshot &panel, const std::uint32_t snapshot_version) {
  PanelModel model;
  if (snapshot_version != core::companion_snapshot_version_typed) {
    for (const auto &item : panel.items) {
      RowModel row;
      row.label = item.name;
      row.value = legacy_value_text(item);
      row.fill = item.progress;
      model.rows.push_back(std::move(row));
    }
    if (model.rows.empty()) model.status = "No cooldowns in this panel.";
    return model;
  }
  if (!panel.visible) {
    model.status = "Hidden by this profile right now.";
    return model;
  }
  for (const auto &item : panel.typed_items) {
    RowModel row;
    row.label = item.label;
    row.value = item.value_text;
    row.pips = item.pips;
    row.filled_pips = item.filled_pips;
    row.color = item.color;
    row.unavailable = item.unavailable;
    if (item.has_progress && !item.unavailable) row.fill = item.fill;
    row.indicator = item.kind == core::OverlayItemKind::indicator;
    row.accent_on = row.indicator && item.active && item.flash_on && !item.unavailable;
    model.rows.push_back(std::move(row));
  }
  if (panel.omitted_items != 0)
    model.status = std::to_string(panel.omitted_items) +
                   (panel.omitted_items == 1 ? " item row" : " item rows") +
                   " omitted to fit the Game Bar snapshot limit.";
  else if (model.rows.empty())
    model.status = "No items in this panel.";
  return model;
}

} // namespace layerc::gamebar::widget
