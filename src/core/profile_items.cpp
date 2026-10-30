#include "profile_reader.hpp"

#include "layerc/core/profile_wire.hpp"
#include "profile_extensions.hpp"
#include "profile_item_codec.hpp"
#include "profile_keys.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace layerc::core::detail {
namespace keys = profile_keys;
namespace {

using json::Value;

std::pair<std::string, Value> member(const std::string_view key, Value value) {
  return {std::string(key), std::move(value)};
}
Value text(const std::string_view value) { return Value(std::string(value)); }

bool allowed_item_fields(const Value &value, const std::string &path,
                         const std::span<const std::string_view> allowed,
                         json::Reader &fields) {
  for (const auto field : known_profile_fields(ProfileObject::item,
                                                profile_schema_version)) {
    if (value.find(field) != nullptr &&
        std::ranges::find(allowed, field) == allowed.end())
      return fields.fail(path + "." + std::string(field),
                         "field is not valid for item kind");
  }
  return true;
}

} // namespace

bool ProfileReader::read_items(const json::Value &value, const std::string &path,
                               Panel &panel) {
  if (!value.is_array()) return fields_.fail(path, "expected array");
  panel.items.emplace();
  panel.items->reserve(value.as_array().size());
  for (std::size_t index{}; index < value.as_array().size(); ++index) {
    OverlayItem item;
    if (!read_item(value.as_array()[index],
                   path + "[" + std::to_string(index) + "]", item))
      return false;
    panel.items->push_back(std::move(item));
  }
  panel.cooldown_ids = overlay_cooldown_projection(*panel.items);
  return true;
}

bool ProfileReader::read_item(const json::Value &value, const std::string &path,
                              OverlayItem &item) {
  if (!value.is_object())
    return fields_.fail(path, "overlay item must be an object");
  std::string kind;
  if (!fields_.string(value, keys::item::kind, path, kind, json::Required::yes,
                      json::EmptyString::allow))
    return false;
  const auto parsed_kind =
      parse_enum_wire(profile_wire::overlay_item_kind, kind);
  if (!parsed_kind.has_value()) {
    item.content = OpaqueOverlayItem{value};
    return true;
  }

  std::optional<OverlayTimerSource> timer_source;
  if (*parsed_kind == OverlayItemKind::timer) {
    std::string source;
    if (!fields_.string(value, keys::item::source, path, source,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    timer_source = parse_enum_wire(profile_wire::overlay_timer_source, source);
    if (!timer_source.has_value())
      return fields_.fail(path + ".source",
                          "expected automation or cooldownActive");
  }

  constexpr std::array cooldown_fields{
      keys::item::id, keys::item::kind, keys::item::label,
      keys::item::cooldown_id};
  constexpr std::array automation_timer_fields{
      keys::item::id, keys::item::kind, keys::item::label,
      keys::item::source, keys::item::timer_id};
  constexpr std::array cooldown_timer_fields{
      keys::item::id, keys::item::kind, keys::item::label,
      keys::item::source, keys::item::cooldown_id};
  constexpr std::array counter_fields{
      keys::item::id, keys::item::kind, keys::item::label,
      keys::item::variable, keys::item::max, keys::item::display};
  constexpr std::array indicator_fields{
      keys::item::id, keys::item::kind, keys::item::label,
      keys::item::variable, keys::item::flash_hz};
  std::span<const std::string_view> allowed;
  switch (*parsed_kind) {
  case OverlayItemKind::cooldown:
    allowed = cooldown_fields;
    break;
  case OverlayItemKind::timer:
    allowed = *timer_source == OverlayTimerSource::automation
        ? std::span<const std::string_view>(automation_timer_fields)
        : std::span<const std::string_view>(cooldown_timer_fields);
    break;
  case OverlayItemKind::counter:
    allowed = counter_fields;
    break;
  case OverlayItemKind::indicator:
    allowed = indicator_fields;
    break;
  }
  if (!allowed_item_fields(value, path, allowed, fields_)) return false;

  if (!fields_.string(value, keys::item::id, path, item.id,
                      json::Required::yes, json::EmptyString::allow))
    return false;
  if (value.find(keys::item::label) != nullptr) {
    std::string label;
    if (!fields_.string(value, keys::item::label, path, label,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    item.label = std::move(label);
  }

  switch (*parsed_kind) {
  case OverlayItemKind::cooldown: {
    std::string cooldown_id;
    if (!fields_.string(value, keys::item::cooldown_id, path, cooldown_id,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    item.content = CooldownItem{std::move(cooldown_id)};
    break;
  }
  case OverlayItemKind::timer:
    if (*timer_source == OverlayTimerSource::automation) {
      std::string timer_id;
      if (!fields_.string(value, keys::item::timer_id, path, timer_id,
                          json::Required::yes, json::EmptyString::allow))
        return false;
      item.content = AutomationTimerItem{std::move(timer_id)};
    } else {
      std::string cooldown_id;
      if (!fields_.string(value, keys::item::cooldown_id, path, cooldown_id,
                          json::Required::yes, json::EmptyString::allow))
        return false;
      item.content = CooldownActiveTimerItem{std::move(cooldown_id)};
    }
    break;
  case OverlayItemKind::counter: {
    CounterItem counter;
    if (!fields_.string(value, keys::item::variable, path, counter.variable,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    if (value.find(keys::item::max) != nullptr) {
      const auto maximum = fields_.number(
          value, keys::item::max, path, std::numeric_limits<double>::lowest(),
          std::numeric_limits<double>::max(), json::Required::yes);
      if (!maximum.has_value()) return false;
      counter.max = *maximum;
    }
    const auto display = fields_.enumeration(
        value, keys::item::display, path,
        std::span<const EnumWireEntry<CounterDisplay>>(
            profile_wire::counter_display),
        json::Required::no, CounterDisplay::number);
    if (!display.has_value()) return false;
    counter.display = *display;
    item.content = std::move(counter);
    break;
  }
  case OverlayItemKind::indicator: {
    IndicatorItem indicator;
    if (!fields_.string(value, keys::item::variable, path, indicator.variable,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    const auto flash = fields_.number(
        value, keys::item::flash_hz, path,
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(), json::Required::no, 0.0);
    if (!flash.has_value()) return false;
    indicator.flash_hz = *flash;
    item.content = std::move(indicator);
    break;
  }
  }
  item.extra_fields = unknown_profile_fields(
      value, ProfileObject::item, profile_schema_version);
  return true;
}

Value serialize_overlay_item(const OverlayItem &item) {
  if (const auto *opaque = std::get_if<OpaqueOverlayItem>(&item.content))
    return opaque->value;

  Value::Object fields{
      member(keys::item::id, text(item.id)),
      member(keys::item::kind,
             text(enum_wire_name(profile_wire::overlay_item_kind,
                                 *overlay_item_kind(item)).value_or(""))),
  };
  if (item.label.has_value())
    fields.emplace(keys::item::label, text(*item.label));

  if (const auto *cooldown = std::get_if<CooldownItem>(&item.content)) {
    fields.emplace(keys::item::cooldown_id, text(cooldown->cooldown_id));
  } else if (const auto *automation_timer =
                 std::get_if<AutomationTimerItem>(&item.content)) {
    fields.emplace(keys::item::source,
                   text(enum_wire_name(profile_wire::overlay_timer_source,
                                       OverlayTimerSource::automation)
                            .value_or("")));
    fields.emplace(keys::item::timer_id, text(automation_timer->timer_id));
  } else if (const auto *active_timer =
                 std::get_if<CooldownActiveTimerItem>(&item.content)) {
    fields.emplace(keys::item::source,
                   text(enum_wire_name(profile_wire::overlay_timer_source,
                                       OverlayTimerSource::cooldown_active)
                            .value_or("")));
    fields.emplace(keys::item::cooldown_id, text(active_timer->cooldown_id));
  } else if (const auto *counter = std::get_if<CounterItem>(&item.content)) {
    fields.emplace(keys::item::variable, text(counter->variable));
    if (counter->max.has_value())
      fields.emplace(keys::item::max, Value(*counter->max));
    fields.emplace(keys::item::display,
                   text(enum_wire_name(profile_wire::counter_display,
                                       counter->display)
                            .value_or("")));
  } else if (const auto *indicator =
                 std::get_if<IndicatorItem>(&item.content)) {
    fields.emplace(keys::item::variable, text(indicator->variable));
    fields.emplace(keys::item::flash_hz, Value(indicator->flash_hz));
  }
  return with_profile_extensions(std::move(fields), item.extra_fields,
                                 ProfileObject::item, profile_schema_version);
}

} // namespace layerc::core::detail
