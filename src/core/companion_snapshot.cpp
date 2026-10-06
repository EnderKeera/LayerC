#include "layerc/core/companion_snapshot.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <chrono>
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

// Companion items mirror the native bars from present_panel(); only the
// progress direction differs (elapsed toward ready instead of remaining).
CompanionSnapshotItem
make_snapshot_item(const BarPresentation &bar) {
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
    panel_snapshot.visible = true;
    const auto presentation =
        present_panel(panel, definitions, states, {}, now);
    for (const auto &bar : presentation.bars) {
      if (panel_snapshot.items.size() >=
          max_companion_snapshot_items)
        break;
      panel_snapshot.items.push_back(make_snapshot_item(bar));
    }
    snapshot.panels.push_back(std::move(panel_snapshot));
  }
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
  CompanionSessionSnapshot snapshot;
  snapshot.profile_id = std::string(profile_id);
  snapshot.session_key = std::string(session_key);
  snapshot.timestamp_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count());
  build_preferences(snapshot, panels, companion);
  build_available_catalog(snapshot, panels);
  build_panel_snapshots(snapshot, panels, definitions, states, now);
  // Invalid runtime data must fail closed, not be erased until it happens to
  // serialize. After validation, removing catalog entries/items preserves all
  // invariants, so subsequent serialization errors can only be the byte bound.
  if (validate_companion_snapshot(snapshot))
    return {};
  auto serialized = serialize_companion_snapshot_json(snapshot);
  if (auto *json = std::get_if<std::string>(&serialized))
    return std::move(*json);

  // Prefer mapped presentation metadata over live cooldown detail when the
  // immutable snapshot exceeds the 64 KiB wire bound. Serialize once per
  // shrink step; do not produce a second JSON copy at the transport boundary.
  drop_unmapped_catalog_entries(snapshot);
  serialized = serialize_companion_snapshot_json(snapshot);
  if (auto *json = std::get_if<std::string>(&serialized))
    return std::move(*json);
  for (auto panel = snapshot.panels.rbegin(); panel != snapshot.panels.rend();
       ++panel) {
    panel->items.clear();
    serialized = serialize_companion_snapshot_json(snapshot);
    if (auto *json = std::get_if<std::string>(&serialized))
      return std::move(*json);
  }
  return {};
}

} // namespace layerc::core
