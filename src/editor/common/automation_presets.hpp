#pragma once

#include "layerc/core/automation.hpp"

#include <cstddef>
#include <string>

namespace layerc::editor {

// Starter automations offered by the editor's add menu.
enum class AutomationPreset {
  blank,
  input_cooldown,
  repeating_session_timer,
  ready_overlay_pulse,
};

[[nodiscard]] layerc::core::AutomationDefinition make_automation_preset(
    AutomationPreset preset, std::string automation_id,
    std::string cooldown_id = {}, std::string panel_id = {});

// Single nodes the editor's palette adds to an existing automation.
enum class AutomationPaletteNode {
  input_rule,
  timer_rule,
  trigger_action,
  timer_action,
  overlay_action,
  flag_variable,
};

[[nodiscard]] layerc::core::AutomationRule
make_palette_rule(AutomationPaletteNode node, std::string rule_id,
                  const std::string &cooldown_id);
// existing_actions numbers generated timer and overlay names.
[[nodiscard]] layerc::core::AutomationAction
make_palette_action(AutomationPaletteNode node, std::size_t existing_actions,
                    const std::string &cooldown_id,
                    const std::string &panel_id);
[[nodiscard]] layerc::core::AutomationVariable
make_palette_variable(std::string variable_id);

} // namespace layerc::editor
