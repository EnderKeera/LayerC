#include "layerc/core/automation.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/profile_wire.hpp"

#include <limits>
#include <map>
#include <optional>
#include <ratio>
#include <string_view>
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
     .min_value = min_bar_width,
     .max_value = max_bar_width,
     .apply = apply_bar_integer<&BarAppearance::width>},
    {.name = "height",
     .type = AutomationValueType::integer,
     .min_value = min_bar_height,
     .max_value = max_bar_height,
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

ResolvedPanelOverrides resolve_panel_overrides(
    const Panel &base,
    const std::span<const OverlayPropertyOverride> overrides) {
  std::map<std::string_view, const OverlayPropertyOverride *> winners;
  for (const auto &item : overrides) {
    if (item.panel_id != base.id) continue;
    const auto found = winners.find(item.property);
    if (found == winners.end() ||
        overlay_override_precedes(item, *found->second))
      winners[item.property] = &item;
  }
  ResolvedPanelOverrides result{.panel = base};
  for (const auto &[property, item] : winners) {
    const auto *spec = find_overlay_property(property);
    if (spec == nullptr) continue;
    if (spec->panel_visibility) {
      if (const auto *visible = std::get_if<bool>(&item->value.storage))
        result.visible = *visible;
    } else if (spec->panel_text) {
      if (const auto *text = std::get_if<std::string>(&item->value.storage))
        result.text = *text;
    } else if (spec->apply != nullptr) {
      spec->apply(result.panel, item->value);
    }
  }
  return result;
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

bool normalize_automation_value(AutomationValue &value,
                                const AutomationValueType expected) noexcept {
  if (!automation_value_type_compatible(value_type(value), expected))
    return false;
  if (expected == AutomationValueType::number)
    if (const auto *integer = std::get_if<std::int64_t>(&value.storage))
      value = AutomationValue(static_cast<double>(*integer));
  return true;
}

std::optional<TimePoint>
automation_time_after(const TimePoint base,
                      const Milliseconds duration) noexcept {
  using NativeDuration = TimePoint::duration;
  using Conversion =
      std::ratio_divide<Milliseconds::period, NativeDuration::period>;
  // MSVC, GCC and MinGW steady clocks have integral sub-millisecond ticks.
  static_assert(Conversion::den == 1);
  constexpr auto maximum = std::numeric_limits<NativeDuration::rep>::max();
  if (duration.count() < 0 || duration.count() > maximum / Conversion::num)
    return std::nullopt;
  const auto ticks = duration.count() * Conversion::num;
  if (base.time_since_epoch().count() > maximum - ticks)
    return std::nullopt;
  return TimePoint(NativeDuration(base.time_since_epoch().count() + ticks));
}

std::optional<Milliseconds> as_duration(const AutomationValue &value) noexcept {
  std::optional<Milliseconds> result;
  if (const auto *duration = std::get_if<DurationValue>(&value.storage))
    result = duration->value;
  else if (const auto *integer = std::get_if<std::int64_t>(&value.storage))
    result = Milliseconds(*integer);
  if (!result || !automation_time_after(TimePoint{}, *result))
    return std::nullopt;
  return result;
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
