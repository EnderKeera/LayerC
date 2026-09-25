#include "layerc/core/automation.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/profile_wire.hpp"

#include <optional>
#include <variant>

namespace layerc::core {
namespace {

// Bounds live on OverlayPropertySpec and are enforced by overlay_value_valid
// before apply runs; these mutate only.
template <bool BarAppearance::*Field>
void apply_bar_flag(Panel &panel, const AutomationValue &value) noexcept {
  if (const auto *boolean = std::get_if<bool>(&value.storage))
    panel.bar.*Field = *boolean;
}

void apply_layout(Panel &panel, const AutomationValue &value) noexcept {
  const auto *text = std::get_if<std::string>(&value.storage);
  if (text == nullptr)
    return;
  if (const auto layout =
          profile_wire::parse(profile_wire::panel_layout, *text))
    panel.layout = *layout;
}

template <std::uint32_t BarAppearance::*Field>
void apply_bar_integer(Panel &panel, const AutomationValue &value) noexcept {
  if (const auto *integer = std::get_if<std::int64_t>(&value.storage))
    panel.bar.*Field = static_cast<std::uint32_t>(*integer);
}

template <double Panel::*Field>
void apply_anchor(Panel &panel, const AutomationValue &value) noexcept {
  if (const auto *number = std::get_if<double>(&value.storage))
    panel.*Field = *number;
}

constexpr OverlayPropertySpec overlay_properties[] = {
    {.name = "visible",
     .type = AutomationValueType::boolean,
     .panel_visibility = true,
     .apply = nullptr},
    {.name = "showText",
     .type = AutomationValueType::boolean,
     .apply = apply_bar_flag<&BarAppearance::show_text>},
    {.name = "hideWhenReady",
     .type = AutomationValueType::boolean,
     .apply = apply_bar_flag<&BarAppearance::hide_when_ready>},
    {.name = "text",
     .type = AutomationValueType::string,
     .panel_text = true,
     .apply = nullptr},
    {.name = "layout",
     .type = AutomationValueType::string,
     .panel_layout = true,
     .apply = apply_layout},
    {.name = "width",
     .type = AutomationValueType::integer,
     .min_value = 40,
     .max_value = 1'200,
     .apply = apply_bar_integer<&BarAppearance::width>},
    {.name = "height",
     .type = AutomationValueType::integer,
     .min_value = 16,
     .max_value = 240,
     .apply = apply_bar_integer<&BarAppearance::height>},
    {.name = "fillColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_bar_integer<&BarAppearance::fill_color>},
    {.name = "backgroundColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_bar_integer<&BarAppearance::background_color>},
    {.name = "borderColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_bar_integer<&BarAppearance::border_color>},
    {.name = "textColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .apply = apply_bar_integer<&BarAppearance::text_color>},
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
    if (!spec.panel_layout)
      return text->size() <= max_automation_string_bytes;
    return profile_wire::parse(profile_wire::panel_layout, *text).has_value();
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
