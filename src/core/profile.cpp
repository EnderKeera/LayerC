#include "layerc/core/profile.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/core/hid.hpp"
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
ProfileError ProfileReader::failure() const {
  return {fields_.error().path, fields_.error().message};
}

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
                        json::Required::yes, json::EmptyString::reject) ||
        !fields_.string(root_, keys::name, "$", profile.name,
                        json::Required::yes, json::EmptyString::reject)) {
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
    // JSON parsing is the single UTF-8 validation boundary. Every persisted
    // string reaches ProfileReader only after that parser succeeds.
    if (!profile.game.global && profile.game.executable_path.empty() &&
        profile.game.executable_name.empty()) {
      return ProfileError{"$.game",
                          "executablePath or executableName is required"};
    }

    std::string visual_runtime = "inline";
    fields_.string(root_, keys::visual_runtime, "$", visual_runtime,
                   json::Required::no, json::EmptyString::allow);
    if (!fields_.ok()) return failure();
    const auto parsed_visual_runtime =
        profile_wire::parse(profile_wire::visual_runtime, visual_runtime);
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
  const auto slot =
      fields_.integer(device, keys::stable_slot, path, 1, 32, json::Required::no,
                      1);
  if (!vid.has_value() || !pid.has_value() || !version.has_value() ||
      !usage_page.has_value() || !usage.has_value() || !slot.has_value() ||
      !fields_.string(device, keys::descriptor_hash, path, out.descriptor_hash,
                      json::Required::yes, json::EmptyString::reject))
    return false;
  fields_.string(device, keys::serial, path, out.serial, json::Required::no,
                 json::EmptyString::allow);
  if (!fields_.ok()) return false;
  if (!is_lowercase_sha256(out.descriptor_hash))
    return fields_.fail(path + ".descriptorHash",
                        "expected a lower-case SHA-256 digest");
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
  const auto threshold =
      fields_.number(control, keys::threshold, path, 0.0, 1.0, json::Required::no,
                     0.5);
  const auto hysteresis =
      fields_.number(control, keys::hysteresis, path, 0.0, 0.49,
                     json::Required::no, 0.1);
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
                      json::Required::yes, json::EmptyString::reject))
    return false;
  const auto parsed_activation =
      profile_wire::parse(profile_wire::hid_activation, activation);
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
  if (value->as_array().size() > 128U)
    return fields_.fail("$.hidBindings",
                        "at most 128 HID bindings are allowed");

  profile.hid_bindings.reserve(value->as_array().size());
  for (std::size_t index = 0; index < value->as_array().size(); ++index) {
    const auto path = "$.hidBindings[" + std::to_string(index) + "]";
    const auto &entry = value->as_array()[index];
    if (!entry.is_object())
      return fields_.fail(path, "HID binding must be an object");
    HidControlBinding binding;
    if (!fields_.string(entry, keys::id, path, binding.id,
                        json::Required::yes, json::EmptyString::reject))
      return false;
    if (binding.id.empty() ||
        !hid_binding_ids_
             .emplace(binding.id, static_cast<std::uint16_t>(index + 1U))
             .second)
      return fields_.fail(path + ".id", "duplicate or empty HID binding id");

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
    if (is_system_keyboard_or_mouse(binding.device) &&
        !binding.allow_system_class)
      return fields_.fail(path + ".allowSystemClass",
                          "keyboard/mouse HID requires explicit opt-in");
    if ((binding.activation == HidActivationMode::axis_positive ||
         binding.activation == HidActivationMode::axis_negative) &&
        binding.hysteresis >
            std::min(binding.threshold, 1.0 - binding.threshold))
      return fields_.fail(path + ".control.hysteresis",
                          "hysteresis crosses the axis threshold");
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

    std::set<ProfileModuleKind> kinds;
    for (std::size_t index = 0; index < value->as_array().size(); ++index) {
      const auto path = "$.modules[" + std::to_string(index) + "]";
      const auto &entry = value->as_array()[index];
      if (!entry.is_object())
        return fields_.fail(path, "module descriptor must be an object");
      std::string kind;
      if (!fields_.string(entry, keys::kind, path, kind, json::Required::yes,
                          json::EmptyString::reject))
        return false;
      ProfileModule module;
      const auto *wire = profile_wire::module(kind);
      if (wire == nullptr)
        return fields_.fail(path + ".kind", "unsupported module kind");
      if (source_schema < wire->minimum_profile_schema)
        return fields_.fail(path + ".kind",
                            "module requires a newer profile schema");
      module.kind = wire->value;
      const auto version = fields_.integer(
          entry, keys::schema_version, path, 1, wire->maximum_version,
          json::Required::yes);
      if (!version.has_value()) return false;
      module.schema_version = static_cast<std::uint32_t>(*version);
      if (!kinds.insert(module.kind).second)
        return fields_.fail(path + ".kind", "duplicate module kind");
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
    if (values->empty()) {
      return fields_.fail("$.cooldowns",
                          "at least one cooldown is required");
    }

    std::set<std::string, std::less<>> ids;
    profile.cooldowns.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      const auto path = "$.cooldowns[" + std::to_string(index) + "]";
      const auto &value = (*values)[index];
      if (!value.is_object()) {
        return fields_.fail(path, "cooldown must be an object");
      }

      CooldownDefinition definition;
      if (!fields_.string(value, keys::id, path, definition.id,
                          json::Required::yes, json::EmptyString::reject) ||
          !fields_.string(value, keys::name, path, definition.name,
                          json::Required::yes, json::EmptyString::reject)) {
        return false;
      }
      if (!ids.insert(definition.id).second) {
        return fields_.fail(path + ".id", "duplicate cooldown id");
      }
      const auto duration = fields_.integer(value, keys::duration_ms, path, 1,
                                            max_cooldown_duration.count(),
                                            json::Required::yes);
      const auto charges = fields_.integer(value, keys::max_charges, path, 1,
                                           99, json::Required::no, 1);
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
                            json::Required::yes, json::EmptyString::reject))
          return false;
        const auto parsed_recovery =
            profile_wire::parse(profile_wire::recovery, recovery);
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
          profile_wire::parse(profile_wire::retrigger, retrigger);
      if (!parsed_retrigger.has_value()) {
        return fields_.fail(path + ".retrigger",
                            "expected 'ignore' or 'restart'");
      }
      definition.retrigger = *parsed_retrigger;
      profile.cooldowns.push_back(std::move(definition));
    }
    cooldown_ids_ = std::move(ids);
    return true;
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
  if (values->as_array().size() > 64U)
    return fields_.fail("$.cooldownGroups", "at most 64 groups are allowed");

  std::set<std::string, std::less<>> group_ids;
  std::set<std::string, std::less<>> shared_members;
  for (std::size_t index{}; index < values->as_array().size(); ++index) {
    const auto path = "$.cooldownGroups[" + std::to_string(index) + "]";
    const auto &value = values->as_array()[index];
    if (!value.is_object())
      return fields_.fail(path, "cooldown group must be an object");
    CooldownGroup group;
    if (!fields_.string(value, keys::id, path, group.id, json::Required::yes,
                        json::EmptyString::reject) ||
        !fields_.string(value, keys::name, path, group.name,
                        json::Required::yes, json::EmptyString::reject))
      return false;
    if (group.id.empty() || !group_ids.insert(group.id).second)
      return fields_.fail(path + ".id", "duplicate or empty group id");

    std::string mode;
    if (!fields_.string(value, keys::mode, path, mode, json::Required::yes,
                        json::EmptyString::reject))
      return false;
    const auto parsed_mode =
        profile_wire::parse(profile_wire::group_mode, mode);
    if (!parsed_mode.has_value())
      return fields_.fail(path + ".mode",
                          "expected sharedPool or triggerPropagation");
    group.mode = *parsed_mode;

    const auto *members =
        fields_.array(value, keys::cooldowns, path, json::Required::yes);
    if (members == nullptr) return false;
    if (members->empty())
      return fields_.fail(path + ".cooldowns", "group requires a member");
    std::set<std::string, std::less<>> local_members;
    for (std::size_t item{}; item < members->size(); ++item) {
      const auto item_path = path + ".cooldowns[" + std::to_string(item) + "]";
      if (!(*members)[item].is_string())
        return fields_.fail(item_path, "expected cooldown id");
      const auto &id = (*members)[item].as_string();
      if (!cooldown_ids_.contains(id))
        return fields_.fail(item_path, "unknown cooldown id");
      if (!local_members.insert(id).second)
        return fields_.fail(item_path, "duplicate group member");
      if (group.mode == CooldownGroupMode::shared_pool &&
          !shared_members.insert(id).second)
        return fields_.fail(item_path,
                            "cooldown belongs to more than one shared pool");
      group.cooldown_ids.push_back(id);
    }

    if (group.mode == CooldownGroupMode::shared_pool) {
      const auto duration = fields_.integer(
          value, keys::duration_ms, path, 1, max_cooldown_duration.count(),
          json::Required::yes);
      const auto charges = fields_.integer(
          value, keys::max_charges, path, 1, 99, json::Required::no, 1);
      std::string recovery;
      std::string retrigger = "ignore";
      if (!duration.has_value() || !charges.has_value() ||
          !fields_.string(value, keys::recovery, path, recovery,
                          json::Required::yes, json::EmptyString::reject))
        return false;
      fields_.string(value, keys::retrigger, path, retrigger,
                     json::Required::no, json::EmptyString::allow);
      if (!fields_.ok()) return false;
      group.duration = Milliseconds(*duration);
      group.max_charges = static_cast<std::uint32_t>(*charges);
      const auto parsed_recovery =
          profile_wire::parse(profile_wire::recovery, recovery);
      if (!parsed_recovery.has_value())
        return fields_.fail(path + ".recovery",
                            "expected single, sequential, or parallel");
      group.recovery = *parsed_recovery;
      const auto parsed_retrigger =
          profile_wire::parse(profile_wire::retrigger, retrigger);
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
    if (values->empty()) {
      return fields_.fail("$.panels", "at least one panel is required");
    }

    std::set<std::string, std::less<>> ids;
    profile.panels.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      const auto path = "$.panels[" + std::to_string(index) + "]";
      const auto &value = (*values)[index];
      if (!value.is_object()) {
        return fields_.fail(path, "panel must be an object");
      }

      Panel panel;
      if (!fields_.string(value, keys::id, path, panel.id,
                          json::Required::yes, json::EmptyString::reject) ||
          !fields_.string(value, keys::name, path, panel.name,
                          json::Required::yes, json::EmptyString::reject)) {
        return false;
      }
      if (!ids.insert(panel.id).second) {
        return fields_.fail(path + ".id", "duplicate panel id");
      }

      std::string layout = "row";
      fields_.string(value, keys::layout, path, layout, json::Required::no,
                     json::EmptyString::allow);
      if (!fields_.ok()) {
        return false;
      }
      const auto parsed_layout =
          profile_wire::parse(profile_wire::panel_layout, layout);
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
          profile_wire::parse(profile_wire::panel_style, style);
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
        const auto width = fields_.integer(
            *bar, keys::width, bar_path, 40, 1'200,
            json::Required::no, panel.bar.width);
        const auto height = fields_.integer(
            *bar, keys::height, bar_path, 16, 240,
            json::Required::no, panel.bar.height);
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

      const auto x = fields_.number(value, keys::anchor_x, path, 0.0, 1.0,
                                    json::Required::no, 0.5);
      const auto y = fields_.number(value, keys::anchor_y, path, 0.0, 1.0,
                                    json::Required::no, 0.5);
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
      if (cooldowns->empty()) {
        return fields_.fail(path + ".cooldowns",
                            "panel requires at least one cooldown id");
      }
      for (std::size_t item = 0; item < cooldowns->size(); ++item) {
        const auto item_path =
            path + ".cooldowns[" + std::to_string(item) + "]";
        if (!(*cooldowns)[item].is_string()) {
          return fields_.fail(item_path, "expected string cooldown id");
        }
        const auto &id = (*cooldowns)[item].as_string();
        if (!cooldown_ids_.contains(id)) {
          return fields_.fail(item_path, "unknown cooldown id");
        }
        panel.cooldown_ids.push_back(id);
      }
      profile.panels.push_back(std::move(panel));
    }
    return true;
  }

bool ProfileReader::read_chord_token(const json::Value &token_value,
                                     const std::string &token_path,
                                     std::set<InputToken> &tokens,
                                     InputBinding &binding) {
  std::string device;
  if (!fields_.string(token_value, keys::device, token_path, device,
                      json::Required::yes, json::EmptyString::reject))
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
                        json::Required::yes, json::EmptyString::reject))
      return false;
    const auto found = hid_binding_ids_.find(binding_id);
    if (found == hid_binding_ids_.end())
      return fields_.fail(token_path + ".bindingId", "unknown HID binding id");
    token.code = found->second;
  } else {
    const std::int64_t maximum = token.device == InputDevice::mouse
                                     ? 7
                                     : token.device == InputDevice::controller
                                           ? 18
                                           : 65'535;
    const auto code = fields_.integer(token_value, keys::code, token_path, 1,
                                      maximum, json::Required::yes);
    const auto controller = fields_.integer(token_value, keys::controller_index,
                                            token_path, 0, 3, json::Required::no,
                                            0);
    if (!code.has_value() || !controller.has_value()) return false;
    if (token.device == InputDevice::controller &&
        !((*code >= 1 && *code <= 10) || (*code >= 13 && *code <= 18)))
      return fields_.fail(token_path + ".code",
                          "controller code must be 1-10 or 13-18");
    token.code = static_cast<std::uint16_t>(*code);
    token.controller_index = static_cast<std::uint8_t>(*controller);
    if (token.device != InputDevice::controller && token.controller_index != 0)
      return fields_.fail(token_path + ".controllerIndex",
                          "controllerIndex is valid only for controller tokens");
  }
  if (!tokens.insert(token).second)
    return fields_.fail(token_path, "duplicate token in chord");
  binding.chord.tokens.push_back(token);
  return true;
}

bool ProfileReader::read_bindings(GameProfile &profile) {
  const auto *values =
      fields_.array(root_, keys::bindings, "$", json::Required::yes);
  if (values == nullptr) return false;
  if (values->empty())
    return fields_.fail("$.bindings", "at least one binding is required");

  profile.bindings.reserve(values->size());
  for (std::size_t index = 0; index < values->size(); ++index) {
    const auto path = "$.bindings[" + std::to_string(index) + "]";
    const auto &value = (*values)[index];
    if (!value.is_object())
      return fields_.fail(path, "binding must be an object");

    InputBinding binding;
    if (!fields_.string(value, keys::cooldown_id, path, binding.cooldown_id,
                        json::Required::yes, json::EmptyString::reject))
      return false;
    if (!cooldown_ids_.contains(binding.cooldown_id))
      return fields_.fail(path + ".cooldownId", "unknown cooldown id");
    const auto *chord =
        fields_.array(value, keys::chord, path, json::Required::yes);
    if (chord == nullptr) return false;
    if (chord->empty())
      return fields_.fail(path + ".chord", "chord requires at least one token");

    std::set<InputToken> tokens;
    for (std::size_t token_index = 0; token_index < chord->size();
         ++token_index) {
      const auto token_path =
          path + ".chord[" + std::to_string(token_index) + "]";
      const auto &token_value = (*chord)[token_index];
      if (!token_value.is_object())
        return fields_.fail(token_path, "input token must be an object");
      if (!read_chord_token(token_value, token_path, tokens, binding))
        return false;
    }
    profile.bindings.push_back(std::move(binding));
  }
  return true;
}
} // namespace layerc::core::detail

namespace layerc::core {

ProfileResult parse_profile_json(const std::string_view source) {
  if (source.size() > max_profile_json_bytes) {
    return ProfileError{.path = "$",
                        .message = "profile exceeds the 256 KiB limit"};
  }
  auto parsed = json::parse_bounded(source, max_profile_json_bytes);
  if (const auto *parse_error = std::get_if<json::FieldError>(&parsed))
    return ProfileError{.path = parse_error->path, .message = parse_error->message};
  return parse_profile_value(std::get<json::Value>(parsed));
}

ProfileResult parse_profile_value(const json::Value &root) {
  return detail::ProfileReader(root).read();
}

} // namespace layerc::core
