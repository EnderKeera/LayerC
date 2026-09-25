#include "editor_automation_internal.hpp"

#include <chrono>
#include <utility>

namespace layerc::editor {
using namespace std::chrono_literals;

layerc::core::AutomationDefinition make_automation_preset(
    const AutomationPreset preset, std::string automation_id,
    std::string cooldown_id, std::string panel_id) {
  layerc::core::AutomationDefinition definition;
  definition.id = std::move(automation_id);
  definition.enabled = true;

  switch (preset) {
  case AutomationPreset::input_cooldown: {
    definition.name = "Input cooldown";
    layerc::core::AutomationRule input;
    input.id = "on-input";
    input.event = layerc::core::AutomationEventKind::input_pressed;
    input.source_filter = "keyboard:18";
    layerc::core::AutomationAction trigger;
    trigger.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    trigger.target = std::move(cooldown_id);
    input.actions.push_back(std::move(trigger));
    definition.rules.push_back(std::move(input));
    break;
  }
  case AutomationPreset::repeating_session_timer: {
    definition.name = "Repeating session timer";
    const auto timer_id = definition.id + "-timer";
    layerc::core::AutomationRule start;
    start.id = "start-timer";
    start.event = layerc::core::AutomationEventKind::session_started;
    layerc::core::AutomationAction timer;
    timer.kind = layerc::core::AutomationActionKind::create_timer;
    timer.target = timer_id;
    timer.value =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    timer.repeating = true;
    start.actions.push_back(std::move(timer));
    layerc::core::AutomationRule tick;
    tick.id = "on-timer";
    tick.event = layerc::core::AutomationEventKind::timer_fired;
    tick.source_filter = timer_id;
    layerc::core::AutomationAction trigger;
    trigger.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    trigger.target = std::move(cooldown_id);
    tick.actions.push_back(std::move(trigger));
    definition.rules = {std::move(start), std::move(tick)};
    break;
  }
  case AutomationPreset::ready_overlay_pulse: {
    definition.name = "Ready overlay pulse";
    layerc::core::AutomationRule ready;
    ready.id = "on-ready";
    ready.event = layerc::core::AutomationEventKind::cooldown_ready;
    ready.source_filter = std::move(cooldown_id);
    layerc::core::AutomationAction overlay;
    overlay.kind = layerc::core::AutomationActionKind::set_overlay_override;
    overlay.target = std::move(panel_id);
    overlay.name = definition.id + "-ready";
    overlay.property = "visible";
    overlay.value = literal(layerc::core::AutomationValue(true));
    overlay.secondary =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    ready.actions.push_back(std::move(overlay));
    definition.rules.push_back(std::move(ready));
    break;
  }
  case AutomationPreset::blank:
  default:
    definition.name = "New automation";
    break;
  }
  return definition;
}

layerc::core::AutomationRule make_palette_rule(const AutomationPaletteNode node,
                                               std::string rule_id,
                                               const std::string &cooldown_id) {
  const bool from_input = node == AutomationPaletteNode::input_rule;
  layerc::core::AutomationRule rule;
  rule.id = std::move(rule_id);
  rule.event = from_input ? layerc::core::AutomationEventKind::input_pressed
                          : layerc::core::AutomationEventKind::timer_fired;
  rule.source_filter = from_input ? "keyboard:18" : "pulse";
  layerc::core::AutomationAction action;
  action.kind = from_input
                    ? layerc::core::AutomationActionKind::trigger_cooldown
                    : layerc::core::AutomationActionKind::reset_cooldown;
  action.target = cooldown_id;
  rule.actions.push_back(std::move(action));
  return rule;
}

layerc::core::AutomationAction
make_palette_action(const AutomationPaletteNode node,
                    const std::size_t existing_actions,
                    const std::string &cooldown_id,
                    const std::string &panel_id) {
  layerc::core::AutomationAction action;
  const auto ordinal = std::to_string(existing_actions + 1U);
  switch (node) {
  case AutomationPaletteNode::timer_action:
    action.kind = layerc::core::AutomationActionKind::create_timer;
    action.target = "pulse-" + ordinal;
    action.value =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    break;
  case AutomationPaletteNode::overlay_action:
    action.kind = layerc::core::AutomationActionKind::set_overlay_override;
    action.target = panel_id;
    action.name = "visibility-" + ordinal;
    action.property = "visible";
    action.value = literal(layerc::core::AutomationValue(true));
    action.secondary =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    break;
  case AutomationPaletteNode::trigger_action:
  default:
    action.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    action.target = cooldown_id;
    break;
  }
  return action;
}

layerc::core::AutomationVariable make_palette_variable(std::string variable_id) {
  return {.id = std::move(variable_id),
          .type = layerc::core::AutomationValueType::boolean,
          .default_value = layerc::core::AutomationValue(false)};
}

} // namespace layerc::editor
