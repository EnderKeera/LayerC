#include "profile_item_validation.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/profile_wire.hpp"
#include "layerc/core/utf8.hpp"
#include "profile_extension_validation.hpp"
#include "profile_keys.hpp"

#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::core::detail {
namespace keys = profile_keys;
namespace {

using IdSet = std::set<std::string, std::less<>>;
using Result = std::optional<ProfileError>;

[[nodiscard]] std::string item_path(const std::size_t panel,
                                    const std::size_t item) {
  return "$.panels[" + std::to_string(panel) + "].items[" +
         std::to_string(item) + "]";
}

[[nodiscard]] Result text(const std::string &path, const std::string_view key,
                          const std::string &value, const bool empty_allowed) {
  const auto field = path + "." + std::string(key);
  if (!empty_allowed && value.empty())
    return ProfileError{field, "expected non-empty string"};
  if (!is_valid_utf8(value))
    return ProfileError{field, "expected valid UTF-8"};
  if (value.size() > max_automation_string_bytes)
    return ProfileError{field, "string exceeds 512 UTF-8 bytes"};
  return std::nullopt;
}

[[nodiscard]] const CooldownDefinition *find_cooldown(
    const GameProfile &profile, const std::string_view id) noexcept {
  for (const auto &cooldown : profile.cooldowns)
    if (cooldown.id == id) return &cooldown;
  return nullptr;
}

[[nodiscard]] Result validate_opaque_item(const OverlayItem &item,
                                          const OpaqueOverlayItem &opaque,
                                          const std::string &path) {
  if (!item.id.empty())
    return ProfileError{path + ".id", "opaque item cannot carry a typed id"};
  if (item.label.has_value())
    return ProfileError{path + ".label", "opaque item cannot carry a typed label"};
  if (!item.extra_fields.empty())
    return ProfileError{path, "opaque item cannot carry typed extensions"};
  if (!opaque.value.is_object())
    return ProfileError{path, "opaque item must be an object"};
  const auto *kind = opaque.value.find(keys::item::kind);
  if (kind == nullptr || !kind->is_string() ||
      parse_enum_wire(profile_wire::overlay_item_kind, kind->as_string()))
    return ProfileError{path + ".kind", "opaque item requires an unknown string kind"};
  return validate_opaque_value(opaque.value, path, 4);
}

[[nodiscard]] Result validate_known_item(const OverlayItem &item,
                                         const std::string &path,
                                         IdSet &ids,
                                         const GameProfile &profile) {
  if (auto error = text(path, keys::item::id, item.id, false)) return error;
  if (item.id.starts_with("@legacy:"))
    return ProfileError{path + ".id", "explicit item id uses the reserved legacy prefix"};
  if (!ids.insert(item.id).second)
    return ProfileError{path + ".id", "duplicate item id"};
  if (item.label.has_value())
    if (auto error = text(path, keys::item::label, *item.label, true)) return error;

  if (const auto *cooldown_item = std::get_if<CooldownItem>(&item.content)) {
    if (auto error = text(path, keys::item::cooldown_id,
                          cooldown_item->cooldown_id, false)) return error;
    if (find_cooldown(profile, cooldown_item->cooldown_id) == nullptr)
      return ProfileError{path + ".cooldownId", "unknown cooldown id"};
  } else if (const auto *automation_timer =
                 std::get_if<AutomationTimerItem>(&item.content)) {
    if (auto error = text(path, keys::item::timer_id,
                          automation_timer->timer_id, false))
      return error;
  } else if (const auto *active_timer =
                 std::get_if<CooldownActiveTimerItem>(&item.content)) {
    if (auto error = text(path, keys::item::cooldown_id,
                          active_timer->cooldown_id, false)) return error;
    const auto *definition = find_cooldown(profile, active_timer->cooldown_id);
    if (definition == nullptr || definition->active_duration.count() <= 0)
      return ProfileError{path + ".cooldownId",
                          "cooldown active timer requires an active duration"};
  } else if (const auto *counter = std::get_if<CounterItem>(&item.content)) {
    if (auto error = text(path, keys::item::variable, counter->variable, false))
      return error;
    if (!enum_wire_name(profile_wire::counter_display, counter->display))
      return ProfileError{path + ".display", "unknown counter display"};
    if (counter->max.has_value() &&
        (!std::isfinite(*counter->max) || *counter->max <= 0.0))
      return ProfileError{path + ".max", "expected a positive finite maximum"};
    if (counter->display == CounterDisplay::pips &&
        (!counter->max.has_value() || std::trunc(*counter->max) != *counter->max ||
         *counter->max < 1.0 || *counter->max > max_counter_pips))
      return ProfileError{path + ".max", "pips require an integer maximum from 1 to 8"};
  } else if (const auto *indicator =
                 std::get_if<IndicatorItem>(&item.content)) {
    if (auto error = text(path, keys::item::variable, indicator->variable, false))
      return error;
    if (!std::isfinite(indicator->flash_hz) ||
        (indicator->flash_hz != 0.0 &&
         (indicator->flash_hz < min_indicator_flash_hz ||
          indicator->flash_hz > max_indicator_flash_hz)))
      return ProfileError{path + ".flashHz", "flash rate is outside the supported range"};
  }
  return std::nullopt;
}

} // namespace

std::optional<ProfileError>
validate_profile_items(const GameProfile &profile) {
  const auto schema = profile_serialization_schema(profile);
  if (schema >= profile_schema_version &&
      profile.panels.size() > max_typed_profile_panels)
    return ProfileError{"$.panels", "at most 64 panels are allowed in schema 5"};

  for (std::size_t panel_index{}; panel_index < profile.panels.size();
       ++panel_index) {
    const auto &panel = profile.panels[panel_index];
    if (!panel.items.has_value()) continue;
    const auto path = "$.panels[" + std::to_string(panel_index) + "].items";
    if (panel.items->empty())
      return ProfileError{path, "typed panel requires at least one item"};
    if (panel.items->size() > max_overlay_items_per_panel)
      return ProfileError{path, "at most 32 items are allowed per typed panel"};
    IdSet ids;
    for (std::size_t item_index{}; item_index < panel.items->size();
         ++item_index) {
      const auto &item = (*panel.items)[item_index];
      const auto current_item_path = item_path(panel_index, item_index);
      if (const auto *opaque = std::get_if<OpaqueOverlayItem>(&item.content)) {
        if (auto error = validate_opaque_item(item, *opaque, current_item_path)) return error;
      } else if (auto error = validate_known_item(item, current_item_path, ids, profile)) {
        return error;
      }
    }
  }
  return std::nullopt;
}

} // namespace layerc::core::detail
