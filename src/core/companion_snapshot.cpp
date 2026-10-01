#include "layerc/core/companion_snapshot.hpp"
#include "layerc/core/cooldown_engine.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>
#include <variant>

namespace layerc::core {
namespace {

void build_preferences(
    layerc::core::CompanionSessionSnapshot &snapshot,
    const std::span<const layerc::core::Panel> panels,
    const std::optional<layerc::core::CompanionProfile> &companion) {
  if (companion.has_value()) {
    snapshot.preferences = companion->slots;
    return;
  }
  for (std::size_t i = 0;
       i < panels.size() && i < layerc::core::max_companion_slots; ++i) {
    snapshot.preferences.push_back(
        {.slot_index = static_cast<std::uint32_t>(i),
         .panel_id = panels[i].id});
  }
}

void build_available_catalog(
    layerc::core::CompanionSessionSnapshot &snapshot,
    const std::span<const layerc::core::Panel> panels) {
  const auto count =
      std::min(panels.size(), layerc::core::max_companion_available_panels);
  snapshot.available_panels.reserve(count);
  for (const auto &panel : panels.first(count))
    snapshot.available_panels.push_back(
        {.panel_id = panel.id,
         .name = panel.name.empty() ? panel.id : panel.name});
}

[[nodiscard]] const layerc::core::CompanionSlotMapping *find_slot_mapping(
    const std::vector<layerc::core::CompanionSlotMapping> &preferences,
    const std::string_view panel_id) {
  const auto found = std::ranges::find_if(preferences, [&](const auto &mapping) {
    return mapping.panel_id == panel_id;
  });
  return found == preferences.end() ? nullptr : &*found;
}

layerc::core::CompanionSnapshotItem
make_snapshot_item(const layerc::core::CooldownDefinition &definition,
                   const layerc::core::CooldownState *state,
                   const layerc::core::TimePoint now) {
  layerc::core::CompanionSnapshotItem item;
  item.cooldown_id = definition.id;
  item.name = definition.name;
  item.max_charges = definition.max_charges;
  if (state == nullptr) {
    item.available_charges = definition.max_charges;
    item.is_ready = true;
    item.remaining_ms = 0;
    item.progress = 1.0;
    return item;
  }
  // Engine states carry the effective owner view. Legacy/manual states may
  // omit it (and max_charges); only those use the supplied member definition.
  if (state->recharge_duration.has_value() || state->max_charges != 0)
    item.max_charges = state->max_charges;
  item.available_charges = state->available_charges;
  item.is_ready =
      state->available_charges > 0 && !state->next_charge_at.has_value();
  item.remaining_ms = cooldown_remaining_ms(state->next_charge_at, now).count();
  // Companion uses elapsed-toward-ready, while native bars show remaining.
  // Both derive from the same effective runtime duration and deadline.
  item.progress = item.remaining_ms == 0
                      ? 1.0
                      : 1.0 - cooldown_progress(definition, *state, now);
  return item;
}

void build_panel_snapshots(
    layerc::core::CompanionSessionSnapshot &snapshot,
    const std::span<const layerc::core::Panel> panels,
    const std::span<const layerc::core::CooldownDefinition> definitions,
    const std::span<const layerc::core::CooldownState> states,
    const layerc::core::TimePoint now) {
  std::unordered_map<std::string, const layerc::core::CooldownState *>
      state_map;
  for (const auto &state : states)
    state_map[state.id] = &state;
  std::unordered_map<std::string, const layerc::core::CooldownDefinition *>
      def_map;
  for (const auto &definition : definitions)
    def_map[definition.id] = &definition;

  for (const auto &panel : panels) {
    if (snapshot.panels.size() >= layerc::core::max_companion_snapshot_panels)
      break;
    const auto *mapping = find_slot_mapping(snapshot.preferences, panel.id);
    if (mapping == nullptr)
      continue;
    layerc::core::CompanionPanelSnapshot panel_snapshot;
    panel_snapshot.panel_id = panel.id;
    panel_snapshot.slot_index = mapping->slot_index;
    panel_snapshot.visible = true;
    for (const auto &item_id : panel.cooldown_ids) {
      if (panel_snapshot.items.size() >=
          layerc::core::max_companion_snapshot_items)
        break;
      const auto def_it = def_map.find(item_id);
      if (def_it == def_map.end())
        continue;
      const auto state_it = state_map.find(item_id);
      panel_snapshot.items.push_back(make_snapshot_item(
          *def_it->second,
          state_it == state_map.end() ? nullptr : state_it->second, now));
    }
    snapshot.panels.push_back(std::move(panel_snapshot));
  }
}

void drop_unmapped_catalog_entries(
    layerc::core::CompanionSessionSnapshot &snapshot) {
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
  layerc::core::CompanionSessionSnapshot snapshot;
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
