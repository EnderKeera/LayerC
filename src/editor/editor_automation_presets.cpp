#include "editor_automation_internal.hpp"

#include <chrono>
#include <utility>

namespace layerc::editor {
namespace {
using namespace std::chrono_literals;

layerc::core::AutomationExpression
literal(layerc::core::AutomationValue value) {
  layerc::core::AutomationExpression expression;
  expression.kind = layerc::core::ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

} // namespace

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

} // namespace layerc::editor
