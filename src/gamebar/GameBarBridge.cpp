#include "GameBarBridge.hpp"

#include <algorithm>
#include <unordered_map>

namespace layerc::gamebar {
namespace {

void build_preferences(
    layerc::core::CompanionSessionSnapshot &snapshot,
    const std::span<const layerc::core::Panel> panels,
    const std::optional<layerc::core::CompanionProfile> &companion) {
  if (companion.has_value()) {
    snapshot.preferences = companion->slots;
  } else {
    for (std::size_t i = 0;
         i < panels.size() && i < layerc::core::max_companion_slots; ++i) {
      snapshot.preferences.push_back(
          {.slot_index = static_cast<std::uint32_t>(i),
           .panel_id = panels[i].id});
    }
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

void build_panel_snapshots(
    layerc::core::CompanionSessionSnapshot &snapshot,
    const std::span<const layerc::core::Panel> panels,
    const std::span<const layerc::core::CooldownDefinition> definitions,
    const std::span<const layerc::core::CooldownState> states,
    const layerc::core::TimePoint now) {
  std::unordered_map<std::string, const layerc::core::CooldownState *>
      state_map;
  for (const auto &s : states)
    state_map[s.id] = &s;
  std::unordered_map<std::string, const layerc::core::CooldownDefinition *>
      def_map;
  for (const auto &d : definitions)
    def_map[d.id] = &d;

  for (const auto &panel : panels) {
    if (snapshot.panels.size() >= layerc::core::max_companion_snapshot_panels)
      break;
    const auto mapping = std::ranges::find_if(
        snapshot.preferences, [&](const layerc::core::CompanionSlotMapping &c) {
          return c.panel_id == panel.id;
        });
    if (mapping == snapshot.preferences.end())
      continue;
    layerc::core::CompanionPanelSnapshot p_snap;
    p_snap.panel_id = panel.id;
    p_snap.slot_index = mapping->slot_index;
    p_snap.visible = true;
    for (const auto &item_id : panel.cooldown_ids) {
      if (p_snap.items.size() >= layerc::core::max_companion_snapshot_items)
        break;
      const auto def_it = def_map.find(item_id);
      if (def_it == def_map.end())
        continue;
      const auto &def = *def_it->second;
      layerc::core::CompanionSnapshotItem item;
      item.cooldown_id = def.id;
      item.name = def.name;
      item.max_charges = def.max_charges;
      const auto state_it = state_map.find(item_id);
      if (state_it != state_map.end()) {
        const auto &s = *state_it->second;
        item.available_charges = s.available_charges;
        item.is_ready =
            s.available_charges > 0 && !s.next_charge_at.has_value();
        if (s.next_charge_at.has_value() && *s.next_charge_at > now) {
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  *s.next_charge_at - now);
          item.remaining_ms = remaining.count();
          const auto total =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  def.duration);
          if (total.count() > 0) {
            item.progress =
                std::clamp(1.0 - (static_cast<double>(remaining.count()) /
                                  static_cast<double>(total.count())),
                           0.0, 1.0);
          }
        } else {
          item.remaining_ms = 0;
          item.progress = 1.0;
        }
      } else {
        item.available_charges = def.max_charges;
        item.is_ready = true;
        item.remaining_ms = 0;
        item.progress = 1.0;
      }
      p_snap.items.push_back(std::move(item));
    }
    snapshot.panels.push_back(std::move(p_snap));
  }
}

} // namespace

layerc::core::CompanionSessionSnapshot GameBarBridge::create_session_snapshot(
    const std::string_view profile_id, const std::string_view session_key,
    const std::span<const layerc::core::Panel> panels,
    const std::span<const layerc::core::CooldownDefinition> definitions,
    const std::span<const layerc::core::CooldownState> states,
    const std::optional<layerc::core::CompanionProfile> &companion,
    const layerc::core::TimePoint now) {
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
  return snapshot;
}

void GameBarBridge::update_snapshot(
    layerc::core::CompanionSessionSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_snapshot_ = std::move(snapshot);
  last_update_time_ = layerc::core::MonotonicClock::now();
}

void GameBarBridge::clear_snapshot() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_snapshot_.reset();
  last_update_time_ = {};
}

std::optional<layerc::core::CompanionSessionSnapshot>
GameBarBridge::latest_snapshot(const layerc::core::TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!latest_snapshot_.has_value())
    return std::nullopt;
  if (now - last_update_time_ > stale_timeout)
    return std::nullopt;
  return latest_snapshot_;
}

std::optional<std::string>
GameBarBridge::latest_snapshot_json(const layerc::core::TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!latest_snapshot_.has_value())
    return std::nullopt;
  if (now - last_update_time_ > stale_timeout)
    return std::nullopt;
  return layerc::core::serialize_companion_snapshot_json(*latest_snapshot_);
}

} // namespace layerc::gamebar
