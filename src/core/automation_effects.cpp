#include "layerc/core/automation.hpp"

#include "layerc/core/cooldown_engine.hpp"

#include <optional>
#include <variant>

namespace layerc::core {
namespace {

void apply_show_text(Panel &panel, const AutomationValue &value) noexcept {
  if (const auto *boolean = std::get_if<bool>(&value.storage))
    panel.bar.show_text = *boolean;
}

void apply_hide_when_ready(Panel &panel,
                           const AutomationValue &value) noexcept {
  if (const auto *boolean = std::get_if<bool>(&value.storage))
    panel.bar.hide_when_ready = *boolean;
}

void apply_layout(Panel &panel, const AutomationValue &value) noexcept {
  const auto *text = std::get_if<std::string>(&value.storage);
  if (text == nullptr)
    return;
  if (*text == "row")
    panel.layout = PanelLayout::row;
  else if (*text == "column")
    panel.layout = PanelLayout::column;
  else if (*text == "grid")
    panel.layout = PanelLayout::grid;
}

void apply_width(Panel &panel, const AutomationValue &value) noexcept {
  const auto *integer = std::get_if<std::int64_t>(&value.storage);
  if (integer != nullptr && *integer >= 40 && *integer <= 1'200)
    panel.bar.width = static_cast<std::uint32_t>(*integer);
}

void apply_height(Panel &panel, const AutomationValue &value) noexcept {
  const auto *integer = std::get_if<std::int64_t>(&value.storage);
  if (integer != nullptr && *integer >= 16 && *integer <= 240)
    panel.bar.height = static_cast<std::uint32_t>(*integer);
}

template <std::uint32_t BarAppearance::*Field>
void apply_color(Panel &panel, const AutomationValue &value) noexcept {
  const auto *integer = std::get_if<std::int64_t>(&value.storage);
  if (integer != nullptr && *integer >= 0 && *integer <= 0xFFFFFF)
    panel.bar.*Field = static_cast<std::uint32_t>(*integer);
}

template <double Panel::*Field>
void apply_anchor(Panel &panel, const AutomationValue &value) noexcept {
  const auto *number = std::get_if<double>(&value.storage);
  if (number != nullptr && *number >= 0.0 && *number <= 1.0)
    panel.*Field = *number;
}

constexpr OverlayPropertySpec overlay_properties[] = {
    {.name = "visible",
     .type = AutomationValueType::boolean,
     .panel_visibility = true,
     .apply = nullptr},
    {.name = "showText",
     .type = AutomationValueType::boolean,
     .apply = apply_show_text},
    {.name = "hideWhenReady",
     .type = AutomationValueType::boolean,
     .apply = apply_hide_when_ready},
    {.name = "text",
     .type = AutomationValueType::string,
     .panel_text = true,
     .apply = nullptr},
    {.name = "layout",
     .type = AutomationValueType::string,
     .string_choices = {"row", "column", "grid"},
     .apply = apply_layout},
    {.name = "width",
     .type = AutomationValueType::integer,
     .min_value = 40,
     .max_value = 1'200,
     .apply = apply_width},
    {.name = "height",
     .type = AutomationValueType::integer,
     .min_value = 16,
     .max_value = 240,
     .apply = apply_height},
    {.name = "fillColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_color<&BarAppearance::fill_color>},
    {.name = "backgroundColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_color<&BarAppearance::background_color>},
    {.name = "borderColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_color<&BarAppearance::border_color>},
    {.name = "textColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_color<&BarAppearance::text_color>},
    {.name = "anchorX",
     .type = AutomationValueType::number,
     .min_value = 0.0,
     .max_value = 1.0,
     .apply = apply_anchor<&Panel::anchor_x>},
    {.name = "anchorY",
     .type = AutomationValueType::number,
     .min_value = 0.0,
     .max_value = 1.0,
     .apply = apply_anchor<&Panel::anchor_y>},
};

} // namespace

const OverlayPropertySpec *
find_overlay_property(const std::string_view name) noexcept {
  for (const auto &spec : overlay_properties)
    if (spec.name == name)
      return &spec;
  return nullptr;
}

bool overlay_value_valid(const OverlayPropertySpec &spec,
                         const AutomationValue &value) noexcept {
  switch (spec.type) {
  case AutomationValueType::boolean:
    return std::get_if<bool>(&value.storage) != nullptr;
  case AutomationValueType::string: {
    const auto *text = std::get_if<std::string>(&value.storage);
    if (text == nullptr)
      return false;
    if (spec.string_choices[0].empty())
      return text->size() <= max_automation_string_bytes;
    return *text == spec.string_choices[0] || *text == spec.string_choices[1] ||
           *text == spec.string_choices[2];
  }
  case AutomationValueType::integer: {
    const auto *integer = std::get_if<std::int64_t>(&value.storage);
    return integer != nullptr &&
           static_cast<double>(*integer) >= spec.min_value &&
           static_cast<double>(*integer) <= spec.max_value;
  }
  case AutomationValueType::number: {
    const auto *number = std::get_if<double>(&value.storage);
    return number != nullptr && *number >= spec.min_value &&
           *number <= spec.max_value;
  }
  default:
    return false;
  }
}

std::optional<Milliseconds> as_duration(const AutomationValue &value) noexcept {
  if (const auto *duration = std::get_if<DurationValue>(&value.storage))
    return duration->value;
  if (const auto *integer = std::get_if<std::int64_t>(&value.storage);
      integer != nullptr && *integer >= 0)
    return Milliseconds(*integer);
  return std::nullopt;
}

bool apply_automation_cooldown_effect(CooldownEngine &cooldowns,
                                      const AutomationEffect &effect,
                                      const TimePoint now,
                                      const std::uint64_t origin_token) {
  switch (effect.kind) {
  case AutomationActionKind::trigger_cooldown:
    (void)cooldowns.trigger_with_propagation(effect.target, now, origin_token);
    return true;
  case AutomationActionKind::reset_cooldown:
    (void)cooldowns.reset(effect.target);
    return true;
  case AutomationActionKind::refund_cooldown:
    (void)cooldowns.refund(effect.target, now);
    return true;
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all: {
    const auto duration = as_duration(effect.value);
    if (!duration.has_value())
      return false;
    (void)cooldowns.reduce(
        effect.target, *duration,
        effect.kind == AutomationActionKind::reduce_cooldown_all, now);
    return true;
  }
  case AutomationActionKind::trigger_group:
    (void)cooldowns.trigger_group(effect.target, now, origin_token);
    return true;
  case AutomationActionKind::reset_group:
    (void)cooldowns.reset_group(effect.target);
    return true;
  default:
    return false;
  }
}

} // namespace layerc::core