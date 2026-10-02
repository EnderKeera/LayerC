#include "layerc/core/panel_presentation.hpp"

#include "layerc/core/cooldown_engine.hpp"

#include <algorithm>

namespace layerc::core {

PanelPresentation present_panel(const Panel &panel,
                                const std::span<const CooldownDefinition> definitions,
                                const std::span<const CooldownState> states,
                                const PanelPresentationOverrides &overrides,
                                const TimePoint now) {
  PanelPresentation result;
  result.bars.reserve(panel.cooldown_ids.size());
  bool any_running{};
  for (const auto &cooldown_id : panel.cooldown_ids) {
    const auto definition =
        std::ranges::find(definitions, cooldown_id, &CooldownDefinition::id);
    if (definition == definitions.end()) continue;
    BarPresentation bar{.cooldown_id = definition->id,
                        .name = definition->name,
                        .label = definition->name,
                        .remaining = 1.0,
                        .remaining_time = {},
                        .available_charges = definition->max_charges,
                        .max_charges = definition->max_charges,
                        .ready = true};
    const auto *state = cooldown_state_at(
        states, static_cast<std::size_t>(definition - definitions.begin()),
        cooldown_id);
    if (state != nullptr) {
      bar.label = cooldown_status_text(*definition, *state, now);
      bar.remaining = cooldown_progress(*state, now);
      bar.remaining_time = cooldown_remaining_ms(state->next_charge_at, now);
      bar.available_charges = state->available_charges;
      bar.max_charges = state->max_charges;
      bar.ready = cooldown_ready(*state);
    }
    if (overrides.text.has_value()) bar.label = *overrides.text;
    any_running = any_running || !bar.ready;
    result.bars.push_back(std::move(bar));
  }

  if (overrides.visible.has_value())
    result.visible = *overrides.visible;
  else
    result.visible = overrides.edit_mode || any_running ||
                     panel.style != PanelStyle::bar || !panel.bar.hide_when_ready;
  return result;
}

} // namespace layerc::core
