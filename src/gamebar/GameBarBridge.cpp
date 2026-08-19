#include "GameBarBridge.hpp"

#include <algorithm>

namespace layerc::gamebar {

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

  std::unordered_map<std::string, const layerc::core::CooldownState *>
      state_map;
  for (const auto &s : states) {
    state_map[s.id] = &s;
  }

  std::unordered_map<std::string, const layerc::core::CooldownDefinition *>
      def_map;
  for (const auto &d : definitions) {
    def_map[d.id] = &d;
  }

  for (std::size_t p_idx = 0; p_idx < panels.size(); ++p_idx) {
    const auto &panel = panels[p_idx];
    layerc::core::CompanionPanelSnapshot p_snap;
    p_snap.panel_id = panel.id;
    p_snap.slot_index = static_cast<std::uint32_t>(p_idx % 8);
    p_snap.visible = true;

    if (companion.has_value()) {
      const auto slot_it = std::ranges::find_if(
          companion->slots,
          [&](const layerc::core::CompanionSlotMapping &mapping) {
            return mapping.panel_id == panel.id;
          });
      if (slot_it != companion->slots.end()) {
        p_snap.slot_index = slot_it->slot_index;
      }
    }

    for (const auto &item_id : panel.cooldown_ids) {
      const auto def_it = def_map.find(item_id);
      if (def_it == def_map.end()) continue;
      const auto &def = *def_it->second;

      layerc::core::CompanionSnapshotItem item;
      item.cooldown_id = def.id;
      item.name = def.name;
      item.max_charges = def.max_charges;

      const auto state_it = state_map.find(item_id);
      if (state_it != state_map.end()) {
        const auto &s = *state_it->second;
        item.available_charges = s.available_charges;
        item.is_ready = s.available_charges > 0 && !s.next_charge_at.has_value();

        if (s.next_charge_at.has_value() && *s.next_charge_at > now) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
              *s.next_charge_at - now);
          item.remaining_ms = remaining.count();
          const auto total_duration =
              std::chrono::duration_cast<std::chrono::milliseconds>(def.duration);
          if (total_duration.count() > 0) {
            item.progress = std::clamp(
                1.0 - (static_cast<double>(remaining.count()) /
                       static_cast<double>(total_duration.count())),
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
  return snapshot;
}

void GameBarBridge::update_snapshot(
    layerc::core::CompanionSessionSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_snapshot_ = std::move(snapshot);
  last_update_time_ = layerc::core::MonotonicClock::now();
}

std::optional<layerc::core::CompanionSessionSnapshot>
GameBarBridge::latest_snapshot(const layerc::core::TimePoint now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!latest_snapshot_.has_value()) {
    return std::nullopt;
  }
  if (now - last_update_time_ > stale_timeout) {
    return std::nullopt; // Hide stale data after 2.0s
  }
  return latest_snapshot_;
}

} // namespace layerc::gamebar
