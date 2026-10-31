#include "layerc/core/companion_snapshot.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <variant>

namespace layerc::core {
namespace {

void build_preferences(
    CompanionSessionSnapshot &snapshot,
    const std::span<const Panel> panels,
    const std::optional<CompanionProfile> &companion) {
  if (companion.has_value()) {
    snapshot.preferences = companion->slots;
    return;
  }
  for (std::size_t i = 0;
       i < panels.size() && i < max_companion_slots; ++i) {
    snapshot.preferences.push_back(
        {.slot_index = static_cast<std::uint32_t>(i),
         .panel_id = panels[i].id});
  }
}

void build_available_catalog(
    CompanionSessionSnapshot &snapshot,
    const std::span<const Panel> panels) {
  const auto count =
      std::min(panels.size(), max_companion_available_panels);
  snapshot.available_panels.reserve(count);
  for (const auto &panel : panels.first(count))
    snapshot.available_panels.push_back(
        {.panel_id = panel.id,
         .name = panel.name.empty() ? panel.id : panel.name});
}

[[nodiscard]] const CompanionSlotMapping *find_slot_mapping(
    const std::vector<CompanionSlotMapping> &preferences,
    const std::string_view panel_id) {
  const auto found = std::ranges::find_if(preferences, [&](const auto &mapping) {
    return mapping.panel_id == panel_id;
  });
  return found == preferences.end() ? nullptr : &*found;
}

// Companion items mirror the native cooldown rows from present_panel(); only
// the progress direction differs (elapsed toward ready instead of remaining).
CompanionSnapshotItem
make_snapshot_item(const ItemPresentation &bar) {
  CompanionSnapshotItem item;
  item.cooldown_id = bar.cooldown_id;
  item.name = bar.name;
  item.progress = bar.ready ? 1.0 : 1.0 - bar.remaining;
  item.available_charges = bar.available_charges;
  item.max_charges = bar.max_charges;
  item.remaining_ms = bar.remaining_time.count();
  item.is_ready = bar.ready;
  return item;
}

void build_panel_snapshots(
    CompanionSessionSnapshot &snapshot,
    const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> definitions,
    const std::span<const CooldownState> states,
    const TimePoint now) {
  for (const auto &panel : panels) {
    if (snapshot.panels.size() >= max_companion_snapshot_panels)
      break;
    const auto *mapping = find_slot_mapping(snapshot.preferences, panel.id);
    if (mapping == nullptr)
      continue;
    CompanionPanelSnapshot panel_snapshot;
    panel_snapshot.panel_id = panel.id;
    panel_snapshot.slot_index = mapping->slot_index;
    const auto presentation =
        present_panel(panel, definitions, states, {}, now);
    for (const auto &bar : presentation.items) {
      // The version-1 wire carries cooldown rows only.
      if (bar.kind != OverlayItemKind::cooldown) continue;
      if (panel_snapshot.items.size() >=
          max_companion_snapshot_items)
        break;
      panel_snapshot.items.push_back(make_snapshot_item(bar));
    }
    snapshot.panels.push_back(std::move(panel_snapshot));
  }
}

CompanionTypedItem make_typed_item(
    const ItemPresentation &row, const PanelPresentationOverrides &options) {
  CompanionTypedItem item;
  item.item_id = row.item_id;
  item.kind = row.kind;
  const auto replacement = std::ranges::find(
      options.items, row.item_id, &ItemPresentationOverride::item_id);
  const bool replaced_text = options.text.has_value() ||
      (replacement != options.items.end() && replacement->text.has_value());
  // Keep the resolved name and value separate: a legal 512-byte name must
  // not overflow the wire text bound when the native row appends its value.
  // A text override replaces the whole row, including its value.
  item.label = replaced_text ? row.label : row.name;
  item.value_text = replaced_text ? std::string{} : row.value_text;
  item.has_progress = row.has_progress;
  item.fill = row.has_progress && std::isfinite(row.remaining)
      ? std::clamp(row.remaining, 0.0, 1.0) : 0.0;
  item.pips = std::min(row.pips, max_counter_pips);
  item.filled_pips = std::min(row.filled_pips, item.pips);
  item.color = row.color & 0xFFFFFFU;
  item.active = row.active;
  item.flash_on = row.flash_on;
  item.unavailable = row.unavailable;
  return item;
}

// Version 2 rows are exactly the native presentation: the same item-scoped
// automation overrides, values, timers and flash phase the overlays draw.
void build_typed_panel_snapshots(
    CompanionSessionSnapshot &snapshot, const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> definitions,
    const std::span<const CooldownState> states,
    const CompanionPresentationInputs &presentation, const TimePoint now,
    std::vector<std::optional<TimePoint>> &deadlines) {
  const auto overrides = presentation.automation != nullptr
      ? std::span<const OverlayPropertyOverride>(presentation.automation->overlay_overrides)
      : std::span<const OverlayPropertyOverride>{};
  for (const auto &panel : panels) {
    if (snapshot.panels.size() >= max_companion_snapshot_panels) break;
    const auto *mapping = find_slot_mapping(snapshot.preferences, panel.id);
    if (mapping == nullptr) continue;
    CompanionPanelSnapshot panel_snapshot;
    panel_snapshot.panel_id = panel.id;
    panel_snapshot.slot_index = mapping->slot_index;
    const auto resolved = resolve_panel_overrides(panel, overrides);
    PanelPresentationOverrides options;
    options.visible = resolved.visible;
    options.text = resolved.text;
    options.presentation_epoch = presentation.presentation_epoch;
    options.items = resolved.items;
    const auto shown = present_panel(resolved.panel, definitions, states,
                                     presentation.automation, options, now);
    panel_snapshot.visible = shown.visible;
    for (const auto &row : shown.items) {
      if (panel_snapshot.typed_items.size() >= max_companion_snapshot_items) {
        ++panel_snapshot.omitted_items;
        continue;
      }
      panel_snapshot.typed_items.push_back(make_typed_item(row, options));
    }
    // Producer-owned cadence: flashing indicators keep their half-period
    // edges, while active countdown text refreshes at <=100 ms.
    auto next = shown.visible ? shown.next_wakeup : std::nullopt;
    if (shown.visible &&
        std::ranges::any_of(shown.items, [](const ItemPresentation &row) {
          return row.active && (row.kind == OverlayItemKind::cooldown ||
                                row.kind == OverlayItemKind::timer);
        }))
      next = earliest_deadline(next, now + Milliseconds{100});
    deadlines.push_back(next);
    snapshot.panels.push_back(std::move(panel_snapshot));
  }
}

[[nodiscard]] std::size_t omitted_rows(const CompanionSessionSnapshot &snapshot) {
  std::size_t total = 0;
  for (const auto &panel : snapshot.panels) total += panel.omitted_items;
  return total;
}

void drop_unmapped_catalog_entries(
    CompanionSessionSnapshot &snapshot) {
  std::erase_if(snapshot.available_panels, [&](const auto &panel) {
    return std::ranges::none_of(snapshot.preferences, [&](const auto &mapping) {
      return mapping.panel_id == panel.panel_id;
    });
  });
}

} // namespace

std::string build_companion_snapshot(
    const std::string_view profile_id, const std::string_view session_key,
    const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> definitions,
    const std::span<const CooldownState> states,
    const std::optional<CompanionProfile> &companion,
    const TimePoint now) {
  return build_companion_snapshot_report(profile_id, session_key, panels, definitions,
                                         states, companion, now, {})
      .json;
}

CompanionSnapshotBuild build_companion_snapshot_report(
    const std::string_view profile_id, const std::string_view session_key,
    const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> definitions,
    const std::span<const CooldownState> states,
    const std::optional<CompanionProfile> &companion, const TimePoint now,
    const CompanionPresentationInputs &presentation) {
  const bool typed = std::ranges::any_of(
      panels, [](const Panel &panel) { return panel.items.has_value(); }) ||
      (presentation.automation != nullptr &&
       std::ranges::any_of(presentation.automation->overlay_overrides,
                          [&](const OverlayPropertyOverride &effect) {
         return !effect.item_id.empty() &&
             std::ranges::any_of(panels, [&](const Panel &panel) {
               return panel.id == effect.panel_id &&
                      panel_item_targetable(panel, effect.item_id);
             });
       }));
  CompanionSessionSnapshot snapshot;
  snapshot.snapshot_version =
      typed ? companion_snapshot_version_typed : companion_snapshot_version_legacy;
  snapshot.profile_id = std::string(profile_id);
  snapshot.session_key = std::string(session_key);
  snapshot.timestamp_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
  build_preferences(snapshot, panels, companion);
  build_available_catalog(snapshot, panels);
  std::vector<std::optional<TimePoint>> deadlines;
  if (typed)
    build_typed_panel_snapshots(snapshot, panels, definitions, states, presentation, now,
                                deadlines);
  else
    build_panel_snapshots(snapshot, panels, definitions, states, now);
  // Invalid runtime data must fail closed, not be erased until it happens to
  // serialize. After validation, removing catalog entries/items preserves all
  // invariants, so subsequent serialization errors can only be the byte bound.
  if (validate_companion_snapshot(snapshot))
    return {};
  const auto report = [&](std::string json) {
    std::optional<TimePoint> next;
    for (std::size_t index = 0; index < deadlines.size(); ++index)
      if (snapshot.panels[index].visible &&
          !snapshot.panels[index].typed_items.empty())
        next = earliest_deadline(next, deadlines[index]);
    return CompanionSnapshotBuild{std::move(json), omitted_rows(snapshot), next};
  };
  auto serialized = serialize_companion_snapshot_json(snapshot);
  if (auto *json = std::get_if<std::string>(&serialized))
    return report(std::move(*json));

  // Prefer mapped presentation metadata over live cooldown detail when the
  // immutable snapshot exceeds the 64 KiB wire bound. Serialize once per
  // shrink step; do not produce a second JSON copy at the transport boundary.
  drop_unmapped_catalog_entries(snapshot);
  serialized = serialize_companion_snapshot_json(snapshot);
  if (auto *json = std::get_if<std::string>(&serialized))
    return report(std::move(*json));
  for (auto panel = snapshot.panels.rbegin(); panel != snapshot.panels.rend();
       ++panel) {
    panel->items.clear();
    panel->omitted_items += static_cast<std::uint32_t>(panel->typed_items.size());
    panel->typed_items.clear();
    serialized = serialize_companion_snapshot_json(snapshot);
    if (auto *json = std::get_if<std::string>(&serialized))
      return report(std::move(*json));
  }
  // Metadata alone cannot fit: publish nothing; the caller keeps the fault.
  return {};
}

} // namespace layerc::core
