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
void apply_bar_flag(ResolvedPanelOverrides &resolved, const AutomationValue &value) noexcept {
  auto &panel = resolved.panel;
  if (const auto *boolean = std::get_if<bool>(&value.storage))
    panel.bar.*Field = *boolean;
}

void apply_layout(ResolvedPanelOverrides &resolved, const AutomationValue &value) noexcept {
  auto &panel = resolved.panel;
  const auto *text = std::get_if<std::string>(&value.storage);
  if (text == nullptr)
    return;
  if (const auto layout =
          parse_enum_wire(profile_wire::panel_layout, *text))
    panel.layout = *layout;
}

template <std::uint32_t BarAppearance::*Field>
void apply_bar_integer(ResolvedPanelOverrides &resolved, const AutomationValue &value) noexcept {
  auto &panel = resolved.panel;
  if (const auto *integer = std::get_if<std::int64_t>(&value.storage))
    panel.bar.*Field = static_cast<std::uint32_t>(*integer);
}

template <double Panel::*Field>
void apply_anchor(ResolvedPanelOverrides &resolved, const AutomationValue &value) noexcept {
  auto &panel = resolved.panel;
  if (const auto *number = std::get_if<double>(&value.storage))
    panel.*Field = *number;
}

bool validate_boolean(const OverlayPropertySpec &, const AutomationValue &value) noexcept {
  return std::holds_alternative<bool>(value.storage);
}
bool validate_text(const OverlayPropertySpec &, const AutomationValue &value) noexcept {
  const auto *text = std::get_if<std::string>(&value.storage);
  return text != nullptr && text->size() <= max_automation_string_bytes;
}
bool validate_layout(const OverlayPropertySpec &, const AutomationValue &value) noexcept {
  const auto *text = std::get_if<std::string>(&value.storage);
  return text != nullptr && parse_enum_wire(profile_wire::panel_layout, *text).has_value();
}
bool validate_integer(const OverlayPropertySpec &spec, const AutomationValue &value) noexcept {
  const auto *integer = std::get_if<std::int64_t>(&value.storage);
  return integer != nullptr && static_cast<double>(*integer) >= spec.min_value &&
         static_cast<double>(*integer) <= spec.max_value;
}
bool validate_number(const OverlayPropertySpec &spec, const AutomationValue &value) noexcept {
  const auto *number = std::get_if<double>(&value.storage);
  return number != nullptr && *number >= spec.min_value && *number <= spec.max_value;
}
void apply_visibility(ResolvedPanelOverrides &resolved, const AutomationValue &value) {
  resolved.visible = std::get<bool>(value.storage);
}
void apply_text(ResolvedPanelOverrides &resolved, const AutomationValue &value) {
  resolved.text = std::get<std::string>(value.storage);
}

constexpr OverlayPropertySpec overlay_properties[] = {
    {.name = "visible",
     .type = AutomationValueType::boolean,
     .validate = validate_boolean,
     .apply = apply_visibility},
    {.name = "showText",
     .type = AutomationValueType::boolean,
     .validate = validate_boolean,
     .apply = apply_bar_flag<&BarAppearance::show_text>},
    {.name = "hideWhenReady",
     .type = AutomationValueType::boolean,
     .validate = validate_boolean,
     .apply = apply_bar_flag<&BarAppearance::hide_when_ready>},
    {.name = "text",
     .type = AutomationValueType::string,
     .validate = validate_text,
     .apply = apply_text},
    {.name = "layout",
     .type = AutomationValueType::string,
     .validate = validate_layout,
     .apply = apply_layout},
    {.name = "width",
     .type = AutomationValueType::integer,
     .min_value = min_bar_width,
     .max_value = max_bar_width,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::width>},
    {.name = "height",
     .type = AutomationValueType::integer,
     .min_value = min_bar_height,
     .max_value = max_bar_height,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::height>},
    {.name = "fillColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::fill_color>},
    {.name = "backgroundColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::background_color>},
    {.name = "borderColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::border_color>},
    {.name = "textColor",
     .type = AutomationValueType::integer,
     .min_value = 0,
     .max_value = 0xFFFFFF,
     .validate = validate_integer,
     .apply = apply_bar_integer<&BarAppearance::text_color>},
    {.name = "anchorX",
     .type = AutomationValueType::number,
     .min_value = 0.0,
     .max_value = 1.0,
     .validate = validate_number,
     .apply = apply_anchor<&Panel::anchor_x>},
    {.name = "anchorY",
     .type = AutomationValueType::number,
     .min_value = 0.0,
     .max_value = 1.0,
     .validate = validate_number,
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
    if (spec != nullptr && overlay_value_valid(*spec, item->value))
      spec->apply(result, item->value);
  }
  return result;
}

bool overlay_value_valid(const OverlayPropertySpec &spec,
                         const AutomationValue &value) noexcept {
  return spec.validate(spec, value);
}

// Shared by the runtime and session admission. Clock overflow remains checked
// against the actual event timestamp when the override is staged.
std::optional<AutomationError> automation_effect_error(
    const AutomationEffect &effect) {
  if (!automation_value_valid(effect.value) ||
      !automation_value_valid(effect.secondary))
    return AutomationError{"$runtime", "action value exceeds automation bounds"};
  if (effect.kind == AutomationActionKind::set_overlay_override) {
    const auto *spec = find_overlay_property(effect.property);
    if (spec == nullptr || !overlay_value_valid(*spec, effect.value))
      return AutomationError{"$runtime", "invalid overlay property value"};
    if (!std::holds_alternative<std::nullptr_t>(effect.secondary.storage) &&
        !as_duration(effect.secondary))
      return AutomationError{"$runtime", "invalid overlay expiry duration"};
  }
  const auto *spec = automation_action_spec(effect.kind);
  if (spec && spec->value == ActionValuePolicy::duration_or_integer) {
    const auto duration = as_duration(effect.value);
    if (spec->domain == AutomationEffectDomain::cooldown_engine) {
      if (!duration)
        return AutomationError{"$runtime", "cooldown reduction is outside the clock range"};
    } else if (!duration || *duration < min_automation_timer_interval) {
      return AutomationError{"$runtime", "timer interval must be at least 50 ms"};
    }
  }
  return std::nullopt;
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


} // namespace layerc::core
