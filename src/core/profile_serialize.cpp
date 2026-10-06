#include "layerc/core/profile.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/core/profile_wire.hpp"
#include "profile_keys.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace layerc::core {
namespace {
namespace keys = profile_keys;
using json::Value;

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
  return Value(Value::Object{
      member(keys::global, Value(game.global)),
      member(keys::executable_path, text(game.executable_path)),
      member(keys::executable_name, text(game.executable_name)),
      member(keys::window_class, text(game.window_class)),
      member(keys::window_title, text(game.window_title)),
  });
}
Value modules_section(const std::vector<ProfileModule> &modules) {
  Value::Array result;
  result.reserve(modules.size());
  for (const auto &module : modules) {
    const auto *descriptor = find_module(module.kind);
    result.emplace_back(Value::Object{
        member(keys::kind, text(descriptor == nullptr ? std::string_view{}
                                                      : descriptor->name)),
        member(keys::schema_version, integer(module.schema_version)),
    });
  }
  return Value(std::move(result));
}
Value cooldowns_section(const std::vector<CooldownDefinition> &cooldowns) {
  Value::Array result;
  result.reserve(cooldowns.size());
  for (const auto &cooldown : cooldowns)
    result.emplace_back(Value::Object{
        member(keys::id, text(cooldown.id)),
        member(keys::name, text(cooldown.name)),
        member(keys::duration_ms, integer(cooldown.duration.count())),
        member(keys::max_charges, integer(cooldown.max_charges)),
        member(keys::retrigger,
               text(enum_wire_name(profile_wire::retrigger,
                                   cooldown.retrigger)
                        .value_or(""))),
        member(keys::recovery,
               text(enum_wire_name(profile_wire::recovery,
                                   cooldown.recovery)
                        .value_or(""))),
    });
  return Value(std::move(result));
}
Value cooldown_groups_section(const std::vector<CooldownGroup> &groups) {
  Value::Array result;
  result.reserve(groups.size());
  for (const auto &group : groups) {
    Value::Object object{
        member(keys::id, text(group.id)),
        member(keys::name, text(group.name)),
        member(keys::mode, text(enum_wire_name(profile_wire::group_mode,
                                               group.mode)
                                    .value_or(""))),
        member(keys::cooldowns, text_list(group.cooldown_ids)),
    };
    if (group.mode == CooldownGroupMode::shared_pool) {
      object.insert(member(
          keys::duration_ms,
          integer(group.duration.count())));
      object.insert(member(keys::max_charges, integer(group.max_charges)));
      object.insert(member(
          keys::recovery,
          text(enum_wire_name(profile_wire::recovery,
                              group.recovery)
                   .value_or(""))));
      object.insert(member(keys::retrigger,
                           text(enum_wire_name(profile_wire::retrigger,
                                               group.retrigger)
                                    .value_or(""))));
    }
    result.emplace_back(std::move(object));
  }
  return Value(std::move(result));
}
Value hid_bindings_section(const std::vector<HidControlBinding> &bindings) {
  Value::Array result;
  result.reserve(bindings.size());
  for (const auto &binding : bindings) {
    Value::Object device{
        member(keys::vid, integer(binding.device.vendor_id)),
        member(keys::pid, integer(binding.device.product_id)),
        member(keys::version, integer(binding.device.version)),
        member(keys::usage_page, integer(binding.device.usage_page)),
        member(keys::usage, integer(binding.device.usage)),
        member(keys::descriptor_hash, text(binding.device.descriptor_hash)),
        member(keys::stable_slot, integer(binding.device.stable_slot)),
    };
    if (!binding.device.serial.empty())
      device.insert(member(keys::serial, text(binding.device.serial)));
    Value::Object control{
        member(keys::report_id, integer(binding.report_id)),
        member(keys::usage_page, integer(binding.usage_page)),
        member(keys::usage, integer(binding.usage)),
        member(keys::link_collection, integer(binding.link_collection)),
        member(keys::activation,
               text(enum_wire_name(profile_wire::hid_activation,
                                   binding.activation)
                        .value_or(""))),
        member(keys::value,
               Value(static_cast<std::int64_t>(binding.discrete_value))),
        member(keys::threshold, Value(binding.threshold)),
        member(keys::hysteresis, Value(binding.hysteresis)),
    };
    result.emplace_back(Value::Object{
        member(keys::id, text(binding.id)),
        member(keys::device, Value(std::move(device))),
        member(keys::control, Value(std::move(control))),
        member(keys::allow_system_class, Value(binding.allow_system_class)),
    });
  }
  return Value(std::move(result));
}
Value panels_section(const std::vector<Panel> &panels) {
  Value::Array result;
  result.reserve(panels.size());
  for (const auto &panel : panels) {
    Value::Object bar{
        member(keys::width, integer(panel.bar.width)),
        member(keys::height, integer(panel.bar.height)),
        member(keys::fill_color, text(color_text(panel.bar.fill_color))),
        member(keys::background_color,
               text(color_text(panel.bar.background_color))),
        member(keys::border_color, text(color_text(panel.bar.border_color))),
        member(keys::text_color, text(color_text(panel.bar.text_color))),
        member(keys::show_text, Value(panel.bar.show_text)),
        member(keys::hide_when_ready, Value(panel.bar.hide_when_ready)),
    };
    result.emplace_back(Value::Object{
        member(keys::id, text(panel.id)),
        member(keys::name, text(panel.name)),
        member(keys::layout,
               text(enum_wire_name(profile_wire::panel_layout,
                                   panel.layout)
                        .value_or(""))),
        member(keys::style,
               text(enum_wire_name(profile_wire::panel_style,
                                   panel.style)
                        .value_or(""))),
        member(keys::bar, Value(std::move(bar))),
        member(keys::anchor_x, Value(panel.anchor_x)),
        member(keys::anchor_y, Value(panel.anchor_y)),
        member(keys::cooldowns, text_list(panel.cooldown_ids)),
    });
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
          member(keys::device, text(input_device_label(token.device))),
      };
      if (token.device == InputDevice::generic_hid) {
        const auto id = token.code > 0 && token.code <= profile.hid_bindings.size()
                            ? profile.hid_bindings[token.code - 1U].id
                            : std::string{};
        object.insert(member(keys::binding_id, text(id)));
      } else {
        object.insert(member(keys::code, integer(token.code)));
        if (token.device == InputDevice::controller)
          object.insert(
              member(keys::controller_index, integer(token.controller_index)));
      }
      chord.emplace_back(std::move(object));
    }
    result.emplace_back(Value::Object{
        member(keys::cooldown_id, text(binding.cooldown_id)),
        member(keys::chord, Value(std::move(chord))),
    });
  }
  return Value(std::move(result));
}

} // namespace

std::string serialize_profile_json(const GameProfile &profile) {
  Value root(Value::Object{
      member(keys::schema_version, integer(profile_schema_version)),
      member(keys::id, text(profile.id)),
      member(keys::name, text(profile.name)),
      member(keys::enabled, Value(profile.enabled)),
      member(keys::game, game_section(profile.game)),
      member(keys::visual_runtime,
             text(enum_wire_name(profile_wire::visual_runtime,
                                 profile.visual_runtime)
                      .value_or(""))),
      member(keys::modules, modules_section(profile.modules)),
      member(keys::cooldowns, cooldowns_section(profile.cooldowns)),
      member(keys::cooldown_groups,
             cooldown_groups_section(profile.cooldown_groups)),
      member(keys::hid_bindings, hid_bindings_section(profile.hid_bindings)),
      member(keys::panels, panels_section(profile.panels)),
      member(keys::bindings, bindings_section(profile)),
  });
  std::string output;
  output.reserve(2'048);
  json::append_value(output, root, 2);
  output += "\r\n";
  return output;
}

} // namespace layerc::core
