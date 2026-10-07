#include "layerc/core/profile.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/core/profile_wire.hpp"

#include "profile_reader.hpp"
#include "profile_keys.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace layerc::core::detail {
namespace keys = profile_keys;

ProfileReader::ProfileReader(const json::Value &root) : root_(root) {}
ProfileError ProfileReader::failure() const { return fields_.error(); }

ProfileResult ProfileReader::read() {
  if (!root_.is_object()) {
    return ProfileError{"$", "profile root must be an object"};
  }

  GameProfile profile;
  const auto schema_version =
      fields_.integer(root_, keys::schema_version, "$", 1,
                      profile_schema_version, json::Required::yes);
  if (!schema_version.has_value()) {
    return failure();
  }
  // Schemas 1 and 2 are migrated in memory. LayerE retains the source schema
  // separately so only an explicit Save can approve the V2.2 boundary.
  profile.schema_version = profile_schema_version;

  if (!fields_.string(root_, keys::id, "$", profile.id,
                      json::Required::yes, json::EmptyString::allow) ||
      !fields_.string(root_, keys::name, "$", profile.name,
                      json::Required::yes, json::EmptyString::allow)) {
    return failure();
  }
  fields_.boolean(root_, keys::enabled, "$", profile.enabled,
                  json::Required::no);
  if (!fields_.ok()) {
    return failure();
  }

  const auto *game =
      fields_.object(root_, keys::game, "$", json::Required::yes);
  if (game == nullptr) {
    return failure();
  }
  fields_.string(*game, keys::executable_path, "$.game",
                 profile.game.executable_path, json::Required::no,
                 json::EmptyString::allow);
  fields_.string(*game, keys::executable_name, "$.game",
                 profile.game.executable_name, json::Required::no,
                 json::EmptyString::allow);
  fields_.string(*game, keys::window_class, "$.game",
                 profile.game.window_class, json::Required::no,
                 json::EmptyString::allow);
  fields_.string(*game, keys::window_title, "$.game",
                 profile.game.window_title, json::Required::no,
                 json::EmptyString::allow);
  fields_.boolean(*game, keys::global, "$.game", profile.game.global,
                  json::Required::no);
  if (!fields_.ok()) {
    return failure();
  }

  std::string visual_runtime = "inline";
  fields_.string(root_, keys::visual_runtime, "$", visual_runtime,
                 json::Required::no, json::EmptyString::allow);
  if (!fields_.ok()) return failure();
  const auto parsed_visual_runtime =
      parse_enum_wire(profile_wire::visual_runtime, visual_runtime);
  if (!parsed_visual_runtime.has_value()) {
    return ProfileError{"$.visualRuntime", "expected 'inline' or 'isolated'"};
  }
  profile.visual_runtime = *parsed_visual_runtime;

  const auto source_schema = static_cast<std::uint32_t>(*schema_version);
  if (!read_modules(profile, source_schema) ||
      !read_hid_bindings(profile, source_schema) ||
      !read_cooldowns(profile, source_schema) ||
      !read_cooldown_groups(profile, source_schema) ||
      !read_panels(profile) || !read_bindings(profile)) {
    return failure();
  }
  return profile;
}

// The reader owns shape, types, wire enums, and which fields each source
// schema carries. Integers are bounded only by their model type; every
// semantic bound, count, uniqueness rule, and reference is
// validate_profile's.

bool ProfileReader::read_hid_device(const json::Value &device,
                                    const std::string &path,
                                    HidDeviceSelector &out) {
  const auto vid =
      fields_.integer(device, keys::vid, path, 0, 65'535, json::Required::yes);
  const auto pid =
      fields_.integer(device, keys::pid, path, 0, 65'535, json::Required::yes);
  const auto version = fields_.integer(device, keys::version, path, 0, 65'535,
                                       json::Required::yes);
  const auto usage_page = fields_.integer(device, keys::usage_page, path, 0,
                                          65'535, json::Required::yes);
  const auto usage =
      fields_.integer(device, keys::usage, path, 0, 65'535, json::Required::yes);
  const auto slot = fields_.integer(device, keys::stable_slot, path, 0, 65'535,
                                    json::Required::no, 1);
  if (!vid.has_value() || !pid.has_value() || !version.has_value() ||
      !usage_page.has_value() || !usage.has_value() || !slot.has_value() ||
      !fields_.string(device, keys::descriptor_hash, path, out.descriptor_hash,
                      json::Required::yes, json::EmptyString::allow))
    return false;
  fields_.string(device, keys::serial, path, out.serial, json::Required::no,
                 json::EmptyString::allow);
  if (!fields_.ok()) return false;
  out.vendor_id = static_cast<std::uint16_t>(*vid);
  out.product_id = static_cast<std::uint16_t>(*pid);
  out.version = static_cast<std::uint16_t>(*version);
  out.usage_page = static_cast<std::uint16_t>(*usage_page);
  out.usage = static_cast<std::uint16_t>(*usage);
  out.stable_slot = static_cast<std::uint16_t>(*slot);
  return true;
}

bool ProfileReader::read_hid_control(const json::Value &control,
                                     const std::string &path,
                                     HidControlBinding &out) {
  const auto report_id =
      fields_.integer(control, keys::report_id, path, 0, 255, json::Required::yes);
  const auto control_page = fields_.integer(control, keys::usage_page, path, 0,
                                            65'535, json::Required::yes);
  const auto control_usage = fields_.integer(control, keys::usage, path, 0,
                                             65'535, json::Required::yes);
  const auto link = fields_.integer(control, keys::link_collection, path, 0,
                                    65'535, json::Required::no, 0);
  const auto discrete = fields_.integer(
      control, keys::value, path, std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max(), json::Required::no, 0);
  constexpr auto lowest = std::numeric_limits<double>::lowest();
  constexpr auto highest = std::numeric_limits<double>::max();
  const auto threshold = fields_.number(control, keys::threshold, path, lowest,
                                        highest, json::Required::no, 0.5);
  const auto hysteresis = fields_.number(control, keys::hysteresis, path,
                                         lowest, highest, json::Required::no,
                                         0.1);
  if (!report_id.has_value() || !control_page.has_value() ||
      !control_usage.has_value() || !link.has_value() ||
      !discrete.has_value() || !threshold.has_value() ||
      !hysteresis.has_value())
    return false;
  out.report_id = static_cast<std::uint8_t>(*report_id);
  out.usage_page = static_cast<std::uint16_t>(*control_page);
  out.usage = static_cast<std::uint16_t>(*control_usage);
  out.link_collection = static_cast<std::uint16_t>(*link);
  out.discrete_value = static_cast<std::int32_t>(*discrete);
  out.threshold = *threshold;
  out.hysteresis = *hysteresis;

  std::string activation;
  if (!fields_.string(control, keys::activation, path, activation,
                      json::Required::yes, json::EmptyString::allow))
    return false;
  const auto parsed_activation =
      parse_enum_wire(profile_wire::hid_activation, activation);
  if (!parsed_activation.has_value())
    return fields_.fail(path + ".activation",
                        "unsupported HID activation mode");
  out.activation = *parsed_activation;
  return true;
}

bool ProfileReader::read_hid_bindings(GameProfile &profile,
                                      const std::uint32_t source_schema) {
  const auto *value = root_.find(keys::hid_bindings);
  if (value == nullptr) {
    if (source_schema >= 3)
      return fields_.fail("$.hidBindings",
                          "schema 3 requires a HID binding collection");
    return true;
  }
  if (source_schema < 3)
    return fields_.fail("$.hidBindings",
                        "generic HID bindings require schema version 3");
  if (!value->is_array())
    return fields_.fail("$.hidBindings", "expected array");

  profile.hid_bindings.reserve(value->as_array().size());
  for (std::size_t index = 0; index < value->as_array().size(); ++index) {
    const auto path = "$.hidBindings[" + std::to_string(index) + "]";
    const auto &entry = value->as_array()[index];
    if (!entry.is_object())
      return fields_.fail(path, "HID binding must be an object");
    HidControlBinding binding;
    if (!fields_.string(entry, keys::id, path, binding.id,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    // Tokens name a binding by its 1-based position. A duplicate keeps the
    // first position; validate_profile rejects the duplicate itself.
    if (index < 65'535U)
      hid_binding_ids_.emplace(binding.id,
                               static_cast<std::uint16_t>(index + 1U));

    const auto *device =
        fields_.object(entry, keys::device, path, json::Required::yes);
    const auto *control =
        fields_.object(entry, keys::control, path, json::Required::yes);
    if (device == nullptr || control == nullptr) return false;
    if (!read_hid_device(*device, path + ".device", binding.device) ||
        !read_hid_control(*control, path + ".control", binding))
      return false;
    fields_.boolean(entry, keys::allow_system_class, path,
                    binding.allow_system_class, json::Required::no);
    if (!fields_.ok()) return false;
    profile.hid_bindings.push_back(std::move(binding));
  }
  return true;
}

bool ProfileReader::read_modules(GameProfile &profile,
                                 const std::uint32_t source_schema) {
  const auto *value = root_.find(keys::modules);
  if (value == nullptr) {
    if (source_schema >= 3)
      return fields_.fail("$.modules", "schema 3 requires module metadata");
    return true;
  }
  if (!value->is_array())
    return fields_.fail("$.modules", "expected array");

  for (std::size_t index = 0; index < value->as_array().size(); ++index) {
    const auto path = "$.modules[" + std::to_string(index) + "]";
    const auto &entry = value->as_array()[index];
    if (!entry.is_object())
      return fields_.fail(path, "module descriptor must be an object");
    std::string kind;
    if (!fields_.string(entry, keys::kind, path, kind, json::Required::yes,
                        json::EmptyString::allow))
      return false;
    ProfileModule module;
    const auto *descriptor = find_module(kind);
    if (descriptor == nullptr)
      return fields_.fail(path + ".kind", "unsupported module kind");
    if (source_schema < descriptor->declarable_from_profile_schema)
      return fields_.fail(path + ".kind",
                          "module requires a newer profile schema");
    module.kind = descriptor->kind;
    const auto version = fields_.integer(
        entry, keys::schema_version, path, 0,
        std::numeric_limits<std::uint32_t>::max(), json::Required::yes);
    if (!version.has_value()) return false;
    module.schema_version = static_cast<std::uint32_t>(*version);
    profile.modules.push_back(module);
  }
  return true;
}

bool ProfileReader::read_cooldowns(GameProfile &profile,
                                   const std::uint32_t source_schema) {
  const auto *values =
      fields_.array(root_, keys::cooldowns, "$", json::Required::yes);
  if (values == nullptr) {
    return false;
  }

  profile.cooldowns.reserve(values->size());
  for (std::size_t index = 0; index < values->size(); ++index) {
    const auto path = "$.cooldowns[" + std::to_string(index) + "]";
    const auto &value = (*values)[index];
    if (!value.is_object()) {
      return fields_.fail(path, "cooldown must be an object");
    }

    CooldownDefinition definition;
    if (!fields_.string(value, keys::id, path, definition.id,
                        json::Required::yes, json::EmptyString::allow) ||
        !fields_.string(value, keys::name, path, definition.name,
                        json::Required::yes, json::EmptyString::allow)) {
      return false;
    }
    const auto duration = read_duration(value, path);
    const auto charges = read_charges(value, path);
    if (!duration.has_value() || !charges.has_value()) {
      return false;
    }
    definition.duration = Milliseconds(*duration);
    definition.max_charges = static_cast<std::uint32_t>(*charges);

    std::string retrigger = "ignore";
    fields_.string(value, keys::retrigger, path, retrigger,
                   json::Required::no, json::EmptyString::allow);
    if (source_schema >= 4) {
      std::string recovery;
      if (!fields_.string(value, keys::recovery, path, recovery,
                          json::Required::yes, json::EmptyString::allow))
        return false;
      const auto parsed_recovery =
          parse_enum_wire(profile_wire::recovery, recovery);
      if (!parsed_recovery.has_value())
        return fields_.fail(path + ".recovery",
                            "expected single, sequential, or parallel");
      definition.recovery = *parsed_recovery;
    } else {
      // Schema <4 expressed recovery as a stackCharges boolean; derive the
      // canonical field here so runtime code never sees the mirror.
      bool stack_charges{};
      fields_.boolean(value, keys::stack_charges, path, stack_charges,
                      json::Required::no);
      definition.recovery = stack_charges ? RecoveryMode::sequential
                                          : RecoveryMode::single;
    }
    if (!fields_.ok()) return false;
    const auto parsed_retrigger =
        parse_enum_wire(profile_wire::retrigger, retrigger);
    if (!parsed_retrigger.has_value()) {
      return fields_.fail(path + ".retrigger",
                          "expected 'ignore' or 'restart'");
    }
    definition.retrigger = *parsed_retrigger;
    profile.cooldowns.push_back(std::move(definition));
  }
  return true;
}

std::optional<std::int64_t>
ProfileReader::read_duration(const json::Value &value, const std::string &path) {
  return fields_.integer(value, keys::duration_ms, path,
                         std::numeric_limits<std::int64_t>::min(),
                         std::numeric_limits<std::int64_t>::max(),
                         json::Required::yes);
}

std::optional<std::int64_t>
ProfileReader::read_charges(const json::Value &value, const std::string &path) {
  return fields_.integer(value, keys::max_charges, path, 0,
                         std::numeric_limits<std::uint32_t>::max(),
                         json::Required::no, 1);
}

bool ProfileReader::read_cooldown_groups(
    GameProfile &profile, const std::uint32_t source_schema) {
  const auto *values = root_.find(keys::cooldown_groups);
  if (values == nullptr) {
    if (source_schema >= 4)
      return fields_.fail("$.cooldownGroups",
                          "schema 4 requires a cooldown group collection");
    return true;
  }
  if (source_schema < 4)
    return fields_.fail("$.cooldownGroups",
                        "cooldown groups require schema version 4");
  if (!values->is_array())
    return fields_.fail("$.cooldownGroups", "expected array");

  for (std::size_t index{}; index < values->as_array().size(); ++index) {
    const auto path = "$.cooldownGroups[" + std::to_string(index) + "]";
    const auto &value = values->as_array()[index];
    if (!value.is_object())
      return fields_.fail(path, "cooldown group must be an object");
    CooldownGroup group;
    if (!fields_.string(value, keys::id, path, group.id, json::Required::yes,
                        json::EmptyString::allow) ||
        !fields_.string(value, keys::name, path, group.name,
                        json::Required::yes, json::EmptyString::allow))
      return false;

    std::string mode;
    if (!fields_.string(value, keys::mode, path, mode, json::Required::yes,
                        json::EmptyString::allow))
      return false;
    const auto parsed_mode =
        parse_enum_wire(profile_wire::group_mode, mode);
    if (!parsed_mode.has_value())
      return fields_.fail(path + ".mode",
                          "expected sharedPool or triggerPropagation");
    group.mode = *parsed_mode;

    const auto *members =
        fields_.array(value, keys::cooldowns, path, json::Required::yes);
    if (members == nullptr) return false;
    for (std::size_t item{}; item < members->size(); ++item) {
      const auto item_path = path + ".cooldowns[" + std::to_string(item) + "]";
      if (!(*members)[item].is_string())
        return fields_.fail(item_path, "expected cooldown id");
      group.cooldown_ids.push_back((*members)[item].as_string());
    }

    if (group.mode == CooldownGroupMode::shared_pool) {
      const auto duration = read_duration(value, path);
      const auto charges = read_charges(value, path);
      std::string recovery;
      std::string retrigger = "ignore";
      if (!duration.has_value() || !charges.has_value() ||
          !fields_.string(value, keys::recovery, path, recovery,
                          json::Required::yes, json::EmptyString::allow))
        return false;
      fields_.string(value, keys::retrigger, path, retrigger,
                     json::Required::no, json::EmptyString::allow);
      if (!fields_.ok()) return false;
      group.duration = Milliseconds(*duration);
      group.max_charges = static_cast<std::uint32_t>(*charges);
      const auto parsed_recovery =
          parse_enum_wire(profile_wire::recovery, recovery);
      if (!parsed_recovery.has_value())
        return fields_.fail(path + ".recovery",
                            "expected single, sequential, or parallel");
      group.recovery = *parsed_recovery;
      const auto parsed_retrigger =
          parse_enum_wire(profile_wire::retrigger, retrigger);
      if (!parsed_retrigger.has_value())
        return fields_.fail(path + ".retrigger",
                            "expected ignore or restart");
      group.retrigger = *parsed_retrigger;
    }
    profile.cooldown_groups.push_back(std::move(group));
  }
  return true;
}
bool ProfileReader::read_panels(GameProfile &profile) {
  const auto *values =
      fields_.array(root_, keys::panels, "$", json::Required::yes);
  if (values == nullptr) {
    return false;
  }

  profile.panels.reserve(values->size());
  for (std::size_t index = 0; index < values->size(); ++index) {
    const auto path = "$.panels[" + std::to_string(index) + "]";
    const auto &value = (*values)[index];
    if (!value.is_object()) {
      return fields_.fail(path, "panel must be an object");
    }

    Panel panel;
    if (!fields_.string(value, keys::id, path, panel.id,
                        json::Required::yes, json::EmptyString::allow) ||
        !fields_.string(value, keys::name, path, panel.name,
                        json::Required::yes, json::EmptyString::allow)) {
      return false;
    }

    std::string layout = "row";
    fields_.string(value, keys::layout, path, layout, json::Required::no,
                   json::EmptyString::allow);
    if (!fields_.ok()) {
      return false;
    }
    const auto parsed_layout =
        parse_enum_wire(profile_wire::panel_layout, layout);
    if (!parsed_layout.has_value()) {
      return fields_.fail(path + ".layout",
                          "expected 'row', 'column', or 'grid'");
    }
    panel.layout = *parsed_layout;

    std::string style = "card";
    fields_.string(value, keys::style, path, style, json::Required::no,
                   json::EmptyString::allow);
    if (!fields_.ok()) {
      return false;
    }
    const auto parsed_style =
        parse_enum_wire(profile_wire::panel_style, style);
    if (!parsed_style.has_value()) {
      return fields_.fail(path + ".style", "expected 'card' or 'bar'");
    }
    panel.style = *parsed_style;

    const auto *bar = value.find(keys::bar);
    if (bar != nullptr) {
      if (!bar->is_object()) {
        return fields_.fail(path + ".bar", "expected object");
      }
      const auto bar_path = path + ".bar";
      constexpr std::int64_t largest = std::numeric_limits<std::uint32_t>::max();
      const auto width = fields_.integer(*bar, keys::width, bar_path, 0,
                                         largest, json::Required::no,
                                         panel.bar.width);
      const auto height = fields_.integer(*bar, keys::height, bar_path, 0,
                                          largest, json::Required::no,
                                          panel.bar.height);
      if (!width.has_value() || !height.has_value()) {
        return false;
      }
      panel.bar.width = static_cast<std::uint32_t>(*width);
      panel.bar.height = static_cast<std::uint32_t>(*height);
      const auto read_color = [&](const std::string_view key,
                                  std::uint32_t &color) {
        if (const auto parsed = fields_.color(*bar, key, bar_path,
                                              json::Required::no, color))
          color = *parsed;
      };
      read_color(keys::fill_color, panel.bar.fill_color);
      read_color(keys::background_color, panel.bar.background_color);
      read_color(keys::border_color, panel.bar.border_color);
      read_color(keys::text_color, panel.bar.text_color);
      fields_.boolean(*bar, keys::show_text, bar_path,
                      panel.bar.show_text, json::Required::no);
      fields_.boolean(*bar, keys::hide_when_ready, bar_path,
                      panel.bar.hide_when_ready, json::Required::no);
      if (!fields_.ok()) {
        return false;
      }
    }

    constexpr auto lowest = std::numeric_limits<double>::lowest();
    constexpr auto highest = std::numeric_limits<double>::max();
    const auto x = fields_.number(value, keys::anchor_x, path, lowest,
                                  highest, json::Required::no, 0.5);
    const auto y = fields_.number(value, keys::anchor_y, path, lowest,
                                  highest, json::Required::no, 0.5);
    if (!x.has_value() || !y.has_value()) {
      return false;
    }
    panel.anchor_x = *x;
    panel.anchor_y = *y;

    const auto *cooldowns =
        fields_.array(value, keys::cooldowns, path, json::Required::yes);
    if (cooldowns == nullptr) {
      return false;
    }
    for (std::size_t item = 0; item < cooldowns->size(); ++item) {
      const auto item_path =
          path + ".cooldowns[" + std::to_string(item) + "]";
      if (!(*cooldowns)[item].is_string()) {
        return fields_.fail(item_path, "expected string cooldown id");
      }
      panel.cooldown_ids.push_back((*cooldowns)[item].as_string());
    }
    profile.panels.push_back(std::move(panel));
  }
  return true;
}

bool ProfileReader::read_chord_token(const json::Value &token_value,
                                     const std::string &token_path,
                                     InputBinding &binding) {
  std::string device;
  if (!fields_.string(token_value, keys::device, token_path, device,
                      json::Required::yes, json::EmptyString::allow))
    return false;
  InputToken token;
  if (const auto parsed = parse_enum_wire(input_device_wire, device))
    token.device = *parsed;
  else
    return fields_.fail(token_path + ".device",
                        "expected keyboard, mouse, controller, or hid");
  if (token.device == InputDevice::generic_hid) {
    std::string binding_id;
    if (!fields_.string(token_value, keys::binding_id, token_path, binding_id,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    // An unknown ID resolves to position 0, which names no binding;
    // validate_profile reports it at bindingId.
    const auto found = hid_binding_ids_.find(binding_id);
    token.code = found == hid_binding_ids_.end() ? 0 : found->second;
  } else {
    const auto code = fields_.integer(token_value, keys::code, token_path, 0,
                                      65'535, json::Required::yes);
    const auto controller = fields_.integer(token_value, keys::controller_index,
                                            token_path, 0, 255,
                                            json::Required::no, 0);
    if (!code.has_value() || !controller.has_value()) return false;
    token.code = static_cast<std::uint16_t>(*code);
    token.controller_index = static_cast<std::uint8_t>(*controller);
  }
  binding.chord.tokens.push_back(token);
  return true;
}

bool ProfileReader::read_bindings(GameProfile &profile) {
  const auto *values =
      fields_.array(root_, keys::bindings, "$", json::Required::yes);
  if (values == nullptr) return false;

  profile.bindings.reserve(values->size());
  for (std::size_t index = 0; index < values->size(); ++index) {
    const auto path = "$.bindings[" + std::to_string(index) + "]";
    const auto &value = (*values)[index];
    if (!value.is_object())
      return fields_.fail(path, "binding must be an object");

    InputBinding binding;
    if (!fields_.string(value, keys::cooldown_id, path, binding.cooldown_id,
                        json::Required::yes, json::EmptyString::allow))
      return false;
    const auto *chord =
        fields_.array(value, keys::chord, path, json::Required::yes);
    if (chord == nullptr) return false;

    for (std::size_t token_index = 0; token_index < chord->size();
         ++token_index) {
      const auto token_path =
          path + ".chord[" + std::to_string(token_index) + "]";
      const auto &token_value = (*chord)[token_index];
      if (!token_value.is_object())
        return fields_.fail(token_path, "input token must be an object");
      if (!read_chord_token(token_value, token_path, binding))
        return false;
    }
    profile.bindings.push_back(std::move(binding));
  }
  return true;
}
} // namespace layerc::core::detail

namespace layerc::core {

ProfileResult parse_profile_json(const std::string_view source) {
  auto parsed = json::parse_bounded(source, max_profile_json_bytes);
  if (const auto *parse_error = std::get_if<json::FieldError>(&parsed))
    return *parse_error;
  return parse_profile_value(std::get<json::Value>(parsed));
}

ProfileResult parse_profile_value(const json::Value &root) {
  auto result = detail::ProfileReader(root).read();
  if (const auto *profile = std::get_if<GameProfile>(&result))
    if (auto error = validate_profile(*profile)) return *std::move(error);
  return result;
}

} // namespace layerc::core
