#pragma once

// Single owner of panel presentation: which panels show and what each bar
// says. Native overlays on Windows and Linux and Game Bar companion snapshots
// all render from present_panel(), so visibility and readiness cannot drift.

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

// The one readiness predicate: no charge is pending recovery.
[[nodiscard]] inline bool cooldown_ready(const CooldownState &state) noexcept {
  return !state.next_charge_at.has_value();
}

struct BarPresentation {
  std::string cooldown_id;
  std::string name;
  // Status text, or the automation text override when one is active.
  std::string label;
  // Native bars show the remaining fraction: 1.0 when ready.
  double remaining{1.0};
  Milliseconds remaining_time{};
  std::uint32_t available_charges{};
  std::uint32_t max_charges{1};
  bool ready{true};
};

struct PanelPresentation {
  bool visible{};
  std::vector<BarPresentation> bars;
};

struct PanelPresentationOverrides {
  std::optional<bool> visible;
  std::optional<std::string> text;
  // Live editing shows every panel so it can be positioned.
  bool edit_mode{};
};

// Visibility: an automation visibility override wins; otherwise edit mode
// shows the panel, and a hide-when-ready bar panel shows only while one of
// its bars is recovering. Every other panel shows, even with no bars.
// `states` follow `definitions` by position (engine snapshot order).
[[nodiscard]] PanelPresentation
present_panel(const Panel &panel, std::span<const CooldownDefinition> definitions,
              std::span<const CooldownState> states,
              const PanelPresentationOverrides &overrides, TimePoint now);

} // namespace layerc::core
