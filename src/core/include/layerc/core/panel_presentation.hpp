#pragma once

// Single owner of panel presentation: which panels show and what each item
// row says. Native overlays on Windows and Linux, editor previews and Game Bar
// companion snapshots all render from present_panel(), so visibility,
// readiness, values and flash phase cannot drift between surfaces.

#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::core {

struct AutomationRuntimeSnapshot;

// The one readiness predicate: no charge is pending recovery.
[[nodiscard]] inline bool cooldown_ready(const CooldownState &state) noexcept {
  return !state.next_charge_at.has_value();
}

// Panel-local runtime IDs of legacy (cooldowns-only) rows. They are never
// persisted, and explicit typed IDs may not use the prefix.
inline constexpr std::string_view legacy_item_id_prefix = "@legacy:";
[[nodiscard]] std::string legacy_item_id(std::size_t index);
// Canonical decimal only: no sign, no leading zero except "0", not empty.
[[nodiscard]] std::optional<std::size_t>
parse_legacy_item_index(std::string_view item_id) noexcept;
// Whether an item-scoped override may target `item_id` on `panel`: a known
// typed item, or an existing legacy row. Opaque items are never targetable.
[[nodiscard]] bool panel_item_targetable(const Panel &panel,
                                         std::string_view item_id) noexcept;

enum class ItemUnavailableReason : std::uint8_t {
  none,
  missing_automation,
  missing_source,
  wrong_type,
  disabled_source,
};

struct ItemPresentation {
  // Explicit typed ID, or "@legacy:<cooldowns index>".
  std::string item_id;
  // Cooldown and cooldown-active timer sources only.
  std::string cooldown_id;
  // Explicit label (possibly empty), else the source name or ID.
  std::string name;
  // The full row text, or the winning text override.
  std::string label;
  // The value part alone: "READY", "3.0s", "5/8", "Unavailable", or empty.
  std::string value_text;
  OverlayItemKind kind{OverlayItemKind::cooldown};
  // Native bars show the remaining fraction: 1.0 when a cooldown is ready.
  double remaining{1.0};
  Milliseconds remaining_time{};
  std::uint32_t available_charges{};
  std::uint32_t max_charges{1};
  std::uint32_t pips{};
  std::uint32_t filled_pips{};
  // Resolved 0xRRGGBB fill, pip and indicator accent.
  std::uint32_t color{};
  bool ready{true};
  bool active{};
  // Core-computed flash phase; true whenever the item is not flashing.
  bool flash_on{true};
  bool unavailable{};
  // False for number-only counters, indicators and unavailable rows.
  bool has_progress{true};
  ItemUnavailableReason unavailable_reason{};

  bool operator==(const ItemPresentation &) const = default;
};

struct PanelPresentation {
  bool visible{};
  std::vector<ItemPresentation> items{};
  // The next instant presentation changes without new input: an active
  // timer's expiry or a flash edge. Running countdown cadence is separate.
  std::optional<TimePoint> next_wakeup{};
};

// Session-level automation overrides of one item.
struct ItemPresentationOverride {
  std::string item_id;
  std::optional<bool> visible{};
  std::optional<std::string> text{};
  std::optional<std::uint32_t> color{};

  bool operator==(const ItemPresentationOverride &) const = default;
};

struct PanelPresentationOverrides {
  std::optional<bool> visible;
  std::optional<std::string> text;
  // Live editing shows every panel so it can be positioned.
  bool edit_mode{};
  // Runtime-only origin of every flash phase: the session's creation time,
  // or a simulator's deterministic start. Phase zero is ON.
  TimePoint presentation_epoch{};
  std::vector<ItemPresentationOverride> items{};
};

// Visibility: an automation visibility override wins; otherwise edit mode
// shows the panel, and a hide-when-ready bar panel shows only while one of
// its rows needs attention (a recovering cooldown, an active timer, a
// nonzero counter, an active indicator or an unavailable item). Every other
// panel shows, even with no rows. `states` follow `definitions` by position
// (engine snapshot order). `automation == nullptr` means no automation
// module is available, so automation-bound items are unavailable.
[[nodiscard]] PanelPresentation
present_panel(const Panel &panel, std::span<const CooldownDefinition> definitions,
              std::span<const CooldownState> states,
              const AutomationRuntimeSnapshot *automation,
              const PanelPresentationOverrides &overrides, TimePoint now);
// Without automation data: automation-bound items report unavailable.
[[nodiscard]] PanelPresentation
present_panel(const Panel &panel, std::span<const CooldownDefinition> definitions,
              std::span<const CooldownState> states,
              const PanelPresentationOverrides &overrides, TimePoint now);

// On-demand, read-only diagnostics for preserved unsupported items and
// unavailable known sources. Independent of visibility overrides; never starts
// automation or mutates profiles. IDs identify the source profile/panel/item.
[[nodiscard]] std::vector<std::string>
overlay_item_diagnostics(const GameProfile &profile,
                         const AutomationRuntimeSnapshot *automation,
                         TimePoint now);

} // namespace layerc::core
