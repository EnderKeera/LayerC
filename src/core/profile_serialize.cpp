#include "layerc/core/profile.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/core/profile_wire.hpp"
#include "profile_keys.hpp"
#include "profile_extensions.hpp"
#include "profile_item_codec.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace layerc::core {
namespace {
namespace keys = profile_keys;
using json::Value;
using detail::ProfileObject;
using detail::serialize_overlay_item;
using detail::with_profile_extensions;

std::pair<std::string, Value> member(const std::string_view key, Value value) {
  return {std::string(key), std::move(value)};
}
Value integer(const std::int64_t value) {
  return Value(value);
}
Value text(const std::string_view value) {
  return Value(std::string(value));
}
Value text_list(const std::vector<std::string> &values) {
  Value::Array result;
  result.reserve(values.size());
  for (const auto &value : values) result.push_back(text(value));
  return Value(std::move(result));
}
using json::color_text;

Value game_section(const GameMatch &game) {
  return with_profile_extensions(Value::Object{
      member(keys::game::global, Value(game.global)),
      member(keys::game::executable_path, text(game.executable_path)),
      member(keys::game::executable_name, text(game.executable_name)),
      member(keys::game::window_class, text(game.window_class)),
      member(keys::game::window_title, text(game.window_title)),
  }, game.extra_fields, ProfileObject::game);
}
Value modules_section(const std::vector<ProfileModule> &modules) {
  Value::Array result;
  result.reserve(modules.size());
  for (const auto &module : modules) {
    const auto *descriptor = find_module(module.kind);
    result.push_back(with_profile_extensions(Value::Object{
        member(keys::module::kind, text(descriptor == nullptr ? std::string_view{}
                                                      : descriptor->name)),
        member(keys::module::schema_version, integer(module.schema_version)),
    }, module.extra_fields, ProfileObject::module));
  }
  return Value(std::move(result));
}
Value cooldowns_section(const std::vector<CooldownDefinition> &cooldowns,
                        const std::uint32_t schema) {
  Value::Array result;
  result.reserve(cooldowns.size());
  for (const auto &cooldown : cooldowns) {
    Value::Object fields{
        member(keys::cooldown::id, text(cooldown.id)),
        member(keys::cooldown::name, text(cooldown.name)),
        member(keys::cooldown::duration_ms, integer(cooldown.duration.count())),
        member(keys::cooldown::max_charges, integer(cooldown.max_charges)),
        member(keys::cooldown::retrigger,
               text(enum_wire_name(profile_wire::retrigger,
                                   cooldown.retrigger)
                        .value_or(""))),
        member(keys::cooldown::recovery,
               text(enum_wire_name(profile_wire::recovery,
                                   cooldown.recovery)
                        .value_or(""))),
    };
    if (cooldown.active_duration.count() != 0)
      fields.emplace(member(keys::cooldown::active_duration_ms,
                            integer(cooldown.active_duration.count())));
    result.push_back(with_profile_extensions(std::move(fields),
        cooldown.extra_fields, ProfileObject::cooldown, schema));
  }
  return Value(std::move(result));
}
Value cooldown_groups_section(const std::vector<CooldownGroup> &groups) {
  Value::Array result;
  result.reserve(groups.size());
  for (const auto &group : groups) {
    Value::Object object{
        member(keys::group::id, text(group.id)),
        member(keys::group::name, text(group.name)),
        member(keys::group::mode, text(enum_wire_name(profile_wire::group_mode,
                                               group.mode)
                                    .value_or(""))),
        member(keys::group::cooldowns, text_list(group.cooldown_ids)),
    };
    if (group.mode == CooldownGroupMode::shared_pool) {
      object.insert(member(
          keys::group::duration_ms,
          integer(group.duration.count())));
      object.insert(member(keys::group::max_charges, integer(group.max_charges)));
      object.insert(member(
          keys::group::recovery,
          text(enum_wire_name(profile_wire::recovery,
                              group.recovery)
                   .value_or(""))));
      object.insert(member(keys::group::retrigger,
                           text(enum_wire_name(profile_wire::retrigger,
                                               group.retrigger)
                                    .value_or(""))));
    }
    result.push_back(with_profile_extensions(std::move(object), group.extra_fields,
                                             ProfileObject::group));
  }
  return Value(std::move(result));
}
Value hid_bindings_section(const std::vector<HidControlBinding> &bindings) {
  Value::Array result;
  result.reserve(bindings.size());
  for (const auto &binding : bindings) {
    Value::Object device{
        member(keys::device::vid, integer(binding.device.vendor_id)),
        member(keys::device::pid, integer(binding.device.product_id)),
        member(keys::device::version, integer(binding.device.version)),
        member(keys::device::usage_page, integer(binding.device.usage_page)),
        member(keys::device::usage, integer(binding.device.usage)),
        member(keys::device::descriptor_hash, text(binding.device.descriptor_hash)),
        member(keys::device::stable_slot, integer(binding.device.stable_slot)),
    };
    if (!binding.device.serial.empty())
      device.insert(member(keys::device::serial, text(binding.device.serial)));
    Value::Object control{
        member(keys::control::report_id, integer(binding.report_id)),
        member(keys::control::usage_page, integer(binding.usage_page)),
        member(keys::control::usage, integer(binding.usage)),
        member(keys::control::link_collection, integer(binding.link_collection)),
        member(keys::control::activation,
               text(enum_wire_name(profile_wire::hid_activation,
                                   binding.activation)
                        .value_or(""))),
        member(keys::control::value,
               Value(static_cast<std::int64_t>(binding.discrete_value))),
        member(keys::control::threshold, Value(binding.threshold)),
        member(keys::control::hysteresis, Value(binding.hysteresis)),
    };
    result.push_back(with_profile_extensions(Value::Object{
        member(keys::hid::id, text(binding.id)),
        member(keys::hid::device, with_profile_extensions(std::move(device),
            binding.device_extra_fields, ProfileObject::device)),
        member(keys::hid::control, with_profile_extensions(std::move(control),
            binding.control_extra_fields, ProfileObject::control)),
        member(keys::hid::allow_system_class, Value(binding.allow_system_class)),
    }, binding.extra_fields, ProfileObject::hid));
  }
  return Value(std::move(result));
}
Value panels_section(const std::vector<Panel> &panels,
                     const std::uint32_t schema) {
  Value::Array result;
  result.reserve(panels.size());
  for (const auto &panel : panels) {
    Value::Object bar{
        member(keys::bar::width, integer(panel.bar.width)),
        member(keys::bar::height, integer(panel.bar.height)),
        member(keys::bar::fill_color, text(color_text(panel.bar.fill_color))),
        member(keys::bar::background_color,
               text(color_text(panel.bar.background_color))),
        member(keys::bar::border_color, text(color_text(panel.bar.border_color))),
        member(keys::bar::text_color, text(color_text(panel.bar.text_color))),
        member(keys::bar::show_text, Value(panel.bar.show_text)),
        member(keys::bar::hide_when_ready, Value(panel.bar.hide_when_ready)),
    };
    Value::Object fields{
        member(keys::panel::id, text(panel.id)),
        member(keys::panel::name, text(panel.name)),
        member(keys::panel::layout,
               text(enum_wire_name(profile_wire::panel_layout,
                                   panel.layout)
                        .value_or(""))),
        member(keys::panel::style,
               text(enum_wire_name(profile_wire::panel_style,
                                   panel.style)
                        .value_or(""))),
        member(keys::panel::bar, with_profile_extensions(std::move(bar),
            panel.bar.extra_fields, ProfileObject::bar, schema)),
        member(keys::panel::anchor_x, Value(panel.anchor_x)),
        member(keys::panel::anchor_y, Value(panel.anchor_y)),
    };
    if (panel.items.has_value()) {
      Value::Array items;
      items.reserve(panel.items->size());
      for (const auto &item : *panel.items)
        items.push_back(serialize_overlay_item(item));
      fields.emplace(member(keys::panel::items, Value(std::move(items))));
      fields.emplace(member(keys::panel::cooldowns,
                            text_list(overlay_cooldown_projection(*panel.items))));
    } else {
      fields.emplace(member(keys::panel::cooldowns, text_list(panel.cooldown_ids)));
    }
    result.push_back(with_profile_extensions(std::move(fields),
        panel.extra_fields, ProfileObject::panel, schema));
  }
  return Value(std::move(result));
}
Value bindings_section(const GameProfile &profile) {
  Value::Array result;
  result.reserve(profile.bindings.size());
  for (const auto &binding : profile.bindings) {
    Value::Array chord;
    chord.reserve(binding.chord.tokens.size());
    for (const auto &token : binding.chord.tokens) {
      Value::Object object{
          member(keys::token::device, text(input_device_label(token.device))),
      };
      if (token.device == InputDevice::generic_hid) {
        const auto id = token.code > 0 && token.code <= profile.hid_bindings.size()
                            ? profile.hid_bindings[token.code - 1U].id
                            : std::string{};
        object.insert(member(keys::token::binding_id, text(id)));
      } else {
        object.insert(member(keys::token::code, integer(token.code)));
        if (token.device == InputDevice::controller)
          object.insert(
              member(keys::token::controller_index, integer(token.controller_index)));
      }
      const auto extra = binding.token_extra_fields.find(token);
      chord.push_back(extra == binding.token_extra_fields.end()
          ? Value(std::move(object))
          : with_profile_extensions(std::move(object), extra->second, ProfileObject::token));
    }
    result.push_back(with_profile_extensions(Value::Object{
        member(keys::binding::cooldown_id, text(binding.cooldown_id)),
        member(keys::binding::chord, Value(std::move(chord))),
    }, binding.extra_fields, ProfileObject::binding));
  }
  return Value(std::move(result));
}

} // namespace

std::string serialize_profile_json(const GameProfile &profile) {
  const auto schema = profile_serialization_schema(profile);
  Value root = with_profile_extensions(Value::Object{
      member(keys::root::schema_version, integer(schema)),
      member(keys::root::id, text(profile.id)),
      member(keys::root::name, text(profile.name)),
      member(keys::root::enabled, Value(profile.enabled)),
      member(keys::root::game, game_section(profile.game)),
      member(keys::root::visual_runtime,
             text(enum_wire_name(profile_wire::visual_runtime,
                                 profile.visual_runtime)
                      .value_or(""))),
      member(keys::root::modules, modules_section(profile.modules)),
      member(keys::root::cooldowns, cooldowns_section(profile.cooldowns, schema)),
      member(keys::root::cooldown_groups,
             cooldown_groups_section(profile.cooldown_groups)),
      member(keys::root::hid_bindings, hid_bindings_section(profile.hid_bindings)),
      member(keys::root::panels, panels_section(profile.panels, schema)),
      member(keys::root::bindings, bindings_section(profile)),
  }, profile.extra_fields, ProfileObject::root, schema);
  std::string output;
  output.reserve(2'048);
  if (!json::append_value_bounded(output, root, max_profile_json_bytes - 2U, 2))
    json::append_value(output, root);
  output += "\r\n";
  return output;
}

} // namespace layerc::core
