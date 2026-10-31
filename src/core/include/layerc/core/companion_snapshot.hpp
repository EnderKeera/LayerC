#pragma once

#include "layerc/core/companion.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace layerc::core {

struct AutomationRuntimeSnapshot;

// Runtime inputs for typed rows: the session's automation snapshot (values,
// timers and item-scoped overrides; null when no automation module) and the
// epoch its flash phases count from.
struct CompanionPresentationInputs {
  const AutomationRuntimeSnapshot *automation{};
  TimePoint presentation_epoch{};
};

struct CompanionSnapshotBuild {
  // Empty when invalid runtime data or metadata alone cannot fit the wire.
  std::string json;
  // Version 2 rows left out to honor the item or byte bound.
  std::size_t omitted_items{};
  // Next producer update needed for visible rows actually retained on wire.
  // No native-overlay dependency; null for static/hidden/unmapped content.
  std::optional<TimePoint> next_wakeup{};
};

// Profiles with typed panels or presentable item-scoped overrides publish
// version 2 rows resolved by present_panel(), including legacy @legacy:<index>
// targets. Cooldown-only profiles without such effects keep the unchanged
// version 1 wire. Oversized valid snapshots shed detail in the documented
// order and count every omitted typed row.
[[nodiscard]] CompanionSnapshotBuild build_companion_snapshot_report(
    std::string_view profile_id, std::string_view session_key,
    std::span<const Panel> panels,
    std::span<const CooldownDefinition> definitions,
    std::span<const CooldownState> states,
    const std::optional<CompanionProfile> &companion, TimePoint now,
    const CompanionPresentationInputs &presentation);

// Build a wire-bounded immutable snapshot from a session's cooldown states.
// Engine states supply effective owner duration/charges; definitions supply
// public IDs/labels (and legacy state defaults). Invalid data returns empty;
// only valid oversize snapshots may drop detail to satisfy the wire bound.
[[nodiscard]] std::string build_companion_snapshot(
    std::string_view profile_id, std::string_view session_key,
    std::span<const Panel> panels,
    std::span<const CooldownDefinition> definitions,
    std::span<const CooldownState> states,
    const std::optional<CompanionProfile> &companion = std::nullopt,
    TimePoint now = MonotonicClock::now());

} // namespace layerc::core
