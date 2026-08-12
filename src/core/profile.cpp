#include "layerc/core/profile.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/core/hid.hpp"

#include "profile_reader.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace layerc::core::detail {

ProfileReader::ProfileReader(const json::Value &root) : root_(root) {}
ProfileResult ProfileReader::read() {
    if (!root_.is_object()) {
      return error("$", "profile root must be an object");
    }

    GameProfile profile;
    const auto schema_version =
        required_integer(root_, "schemaVersion", "$", 1, 3);
    if (!schema_version.has_value()) {
      return error_;
    }
    // Schemas 1 and 2 are migrated in memory. LayerE retains the source schema
    // separately so only an explicit Save can approve the V2.2 boundary.
    profile.schema_version = 3;

    if (!required_string(root_, "id", "$", profile.id) ||
        !required_string(root_, "name", "$", profile.name)) {
      return error_;
    }
    optional_boolean(root_, "enabled", "$", profile.enabled);
    if (!ok()) {
      return error_;
    }

    const auto *game = required_object(root_, "game", "$");
    if (game == nullptr) {
      return error_;
    }
    optional_string(*game, "executablePath", "$.game", profile.game.executable_path);
    optional_string(*game, "executableName", "$.game", profile.game.executable_name);
    optional_string(*game, "windowClass", "$.game", profile.game.window_class);
    optional_string(*game, "windowTitle", "$.game", profile.game.window_title);
    optional_boolean(*game, "global", "$.game", profile.game.global);
    if (!ok()) {
      return error_;
    }
    // JSON parsing is the single UTF-8 validation boundary. Every persisted
    // string reaches ProfileReader only after that parser succeeds.
    if (!profile.game.global && profile.game.executable_path.empty() &&
        profile.game.executable_name.empty()) {
      return error("$.game", "executablePath or executableName is required");
    }

    std::string visual_runtime = "inline";
    optional_string(root_, "visualRuntime", "$", visual_runtime);
    if (!ok()) return error_;
    if (visual_runtime == "inline") {
      profile.visual_runtime = VisualRuntime::inline_process;
    } else if (visual_runtime == "isolated") {
      profile.visual_runtime = VisualRuntime::isolated;
    } else {
      return error("$.visualRuntime", "expected 'inline' or 'isolated'");
    }

    if (!read_modules(profile, static_cast<std::uint32_t>(*schema_version)) ||
        !read_hid_bindings(profile,
                           static_cast<std::uint32_t>(*schema_version)) ||
        !read_cooldowns(profile) || !read_panels(profile) ||
        !read_bindings(profile)) {
      return error_;
    }
    return profile;
  }

bool ProfileReader::read_hid_bindings(GameProfile &profile,
                                      const std::uint32_t source_schema) {
    const auto *value = root_.find("hidBindings");
    if (value == nullptr) {
      if (source_schema >= 3)
        return set_error("$.hidBindings",
                         "schema 3 requires a HID binding collection");
      return true;
    }
    if (source_schema < 3)
      return set_error("$.hidBindings",
                       "generic HID bindings require schema version 3");
    if (!value->is_array())
      return set_error("$.hidBindings", "expected array");
    if (value->as_array().size() > 128U)
      return set_error("$.hidBindings", "at most 128 HID bindings are allowed");

    profile.hid_bindings.reserve(value->as_array().size());
    for (std::size_t index = 0; index < value->as_array().size(); ++index) {
      const auto path = "$.hidBindings[" + std::to_string(index) + "]";
      const auto &entry = value->as_array()[index];
      if (!entry.is_object())
        return set_error(path, "HID binding must be an object");
      HidControlBinding binding;
      if (!required_string(entry, "id", path, binding.id)) return false;
      if (binding.id.empty() ||
          !hid_binding_ids_
               .emplace(binding.id,
                        static_cast<std::uint16_t>(index + 1U))
               .second)
        return set_error(path + ".id", "duplicate or empty HID binding id");

      const auto *device = required_object(entry, "device", path);
      const auto *control = required_object(entry, "control", path);
      if (device == nullptr || control == nullptr) return false;
      const auto vid =
          required_integer(*device, "vid", path + ".device", 0, 65'535);
      const auto pid =
          required_integer(*device, "pid", path + ".device", 0, 65'535);
      const auto version =
          required_integer(*device, "version", path + ".device", 0, 65'535);
      const auto usage_page = required_integer(
          *device, "usagePage", path + ".device", 0, 65'535);
      const auto usage =
          required_integer(*device, "usage", path + ".device", 0, 65'535);
      const auto slot = optional_integer(*device, "stableSlot",
                                         path + ".device", 1, 32, 1);
      if (!vid.has_value() || !pid.has_value() || !version.has_value() ||
          !usage_page.has_value() || !usage.has_value() ||
          !slot.has_value() ||
          !required_string(*device, "descriptorHash", path + ".device",
                           binding.device.descriptor_hash))
        return false;
      optional_string(*device, "serial", path + ".device",
                      binding.device.serial);
      if (!ok()) return false;
      if (!is_lowercase_sha256(binding.device.descriptor_hash))
        return set_error(path + ".device.descriptorHash",
                         "expected a lower-case SHA-256 digest");
      binding.device.vendor_id = static_cast<std::uint16_t>(*vid);
      binding.device.product_id = static_cast<std::uint16_t>(*pid);
      binding.device.version = static_cast<std::uint16_t>(*version);
      binding.device.usage_page = static_cast<std::uint16_t>(*usage_page);
      binding.device.usage = static_cast<std::uint16_t>(*usage);
      binding.device.stable_slot = static_cast<std::uint16_t>(*slot);

      const auto report_id =
          required_integer(*control, "reportId", path + ".control", 0, 255);
      const auto control_page = required_integer(
          *control, "usagePage", path + ".control", 0, 65'535);
      const auto control_usage =
          required_integer(*control, "usage", path + ".control", 0, 65'535);
      const auto link = optional_integer(*control, "linkCollection",
                                         path + ".control", 0, 65'535, 0);
      const auto discrete = optional_integer(
          *control, "value", path + ".control",
          std::numeric_limits<std::int32_t>::min(),
          std::numeric_limits<std::int32_t>::max(), 0);
      const auto threshold = optional_number(*control, "threshold",
                                             path + ".control", 0.0, 1.0, 0.5);
      const auto hysteresis = optional_number(
          *control, "hysteresis", path + ".control", 0.0, 0.49, 0.1);
      if (!report_id.has_value() || !control_page.has_value() ||
          !control_usage.has_value() || !link.has_value() ||
          !discrete.has_value() || !threshold.has_value() ||
          !hysteresis.has_value())
        return false;
      binding.report_id = static_cast<std::uint8_t>(*report_id);
      binding.usage_page = static_cast<std::uint16_t>(*control_page);
      binding.usage = static_cast<std::uint16_t>(*control_usage);
      binding.link_collection = static_cast<std::uint16_t>(*link);
      binding.discrete_value = static_cast<std::int32_t>(*discrete);
      binding.threshold = *threshold;
      binding.hysteresis = *hysteresis;

      std::string activation;
      if (!required_string(*control, "activation", path + ".control",
                           activation))
        return false;
      if (activation == "button")
        binding.activation = HidActivationMode::button;
      else if (activation == "equals")
        binding.activation = HidActivationMode::equals;
      else if (activation == "axisPositive")
        binding.activation = HidActivationMode::axis_positive;
      else if (activation == "axisNegative")
        binding.activation = HidActivationMode::axis_negative;
      else if (activation == "relativePositive")
        binding.activation = HidActivationMode::relative_positive;
      else if (activation == "relativeNegative")
        binding.activation = HidActivationMode::relative_negative;
      else
        return set_error(path + ".control.activation",
                         "unsupported HID activation mode");
      optional_boolean(entry, "allowSystemClass", path,
                       binding.allow_system_class);
      if (!ok()) return false;
      if (is_system_keyboard_or_mouse(binding.device) &&
          !binding.allow_system_class)
        return set_error(path + ".allowSystemClass",
                         "keyboard/mouse HID requires explicit opt-in");
      if ((binding.activation == HidActivationMode::axis_positive ||
           binding.activation == HidActivationMode::axis_negative) &&
          binding.hysteresis >
              std::min(binding.threshold, 1.0 - binding.threshold))
        return set_error(path + ".control.hysteresis",
                         "hysteresis crosses the axis threshold");
      profile.hid_bindings.push_back(std::move(binding));
    }
    return true;
  }

bool ProfileReader::read_modules(GameProfile &profile,
                                 const std::uint32_t source_schema) {
    const auto *value = root_.find("modules");
    if (value == nullptr) {
      if (source_schema >= 3)
        return set_error("$.modules", "schema 3 requires module metadata");
      return true;
    }
    if (!value->is_array())
      return set_error("$.modules", "expected array");

    std::set<ProfileModuleKind> kinds;
    for (std::size_t index = 0; index < value->as_array().size(); ++index) {
      const auto path = "$.modules[" + std::to_string(index) + "]";
      const auto &entry = value->as_array()[index];
      if (!entry.is_object())
        return set_error(path, "module descriptor must be an object");
      std::string kind;
      if (!required_string(entry, "kind", path, kind)) return false;
      ProfileModule module;
      if (kind == "visual") {
        module.kind = ProfileModuleKind::visual;
      } else {
        return set_error(path + ".kind", "unsupported module kind");
      }
      const auto version =
          required_integer(entry, "schemaVersion", path, 1, 2);
      if (!version.has_value()) return false;
      module.schema_version = static_cast<std::uint32_t>(*version);
      if (!kinds.insert(module.kind).second)
        return set_error(path + ".kind", "duplicate module kind");
      profile.modules.push_back(module);
    }
    return true;
  }

bool ProfileReader::read_cooldowns(GameProfile &profile) {
    const auto *values = required_array(root_, "cooldowns", "$");
    if (values == nullptr) {
      return false;
    }
    if (values->empty()) {
      return set_error("$.cooldowns", "at least one cooldown is required");
    }

    std::set<std::string, std::less<>> ids;
    profile.cooldowns.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      const auto path = "$.cooldowns[" + std::to_string(index) + "]";
      const auto &value = (*values)[index];
      if (!value.is_object()) {
        return set_error(path, "cooldown must be an object");
      }

      CooldownDefinition definition;
      if (!required_string(value, "id", path, definition.id) ||
          !required_string(value, "name", path, definition.name)) {
        return false;
      }
      if (!ids.insert(definition.id).second) {
        return set_error(path + ".id", "duplicate cooldown id");
      }
      const auto duration = required_integer(value, "durationMs", path, 1, 86'400'000);
      const auto charges = optional_integer(value, "maxCharges", path, 1, 99, 1);
      if (!duration.has_value() || !charges.has_value()) {
        return false;
      }
      definition.duration = Milliseconds(*duration);
      definition.max_charges = static_cast<std::uint32_t>(*charges);

      std::string retrigger = "ignore";
      optional_string(value, "retrigger", path, retrigger);
      optional_boolean(value, "stackCharges", path, definition.stack_charges);
      if (!ok()) {
        return false;
      }
      if (retrigger == "ignore") {
        definition.retrigger = RetriggerPolicy::ignore;
      } else if (retrigger == "restart") {
        definition.retrigger = RetriggerPolicy::restart;
      } else {
        return set_error(path + ".retrigger", "expected 'ignore' or 'restart'");
      }
      profile.cooldowns.push_back(std::move(definition));
    }
    cooldown_ids_ = std::move(ids);
    return true;
  }

bool ProfileReader::read_panels(GameProfile &profile) {
    const auto *values = required_array(root_, "panels", "$");
    if (values == nullptr) {
      return false;
    }
    if (values->empty()) {
      return set_error("$.panels", "at least one panel is required");
    }

    std::set<std::string, std::less<>> ids;
    profile.panels.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      const auto path = "$.panels[" + std::to_string(index) + "]";
      const auto &value = (*values)[index];
      if (!value.is_object()) {
        return set_error(path, "panel must be an object");
      }

      Panel panel;
      if (!required_string(value, "id", path, panel.id) ||
          !required_string(value, "name", path, panel.name)) {
        return false;
      }
      if (!ids.insert(panel.id).second) {
        return set_error(path + ".id", "duplicate panel id");
      }

      std::string layout = "row";
      optional_string(value, "layout", path, layout);
      if (!ok()) {
        return false;
      }
      if (layout == "row") {
        panel.layout = PanelLayout::row;
      } else if (layout == "column") {
        panel.layout = PanelLayout::column;
      } else if (layout == "grid") {
        panel.layout = PanelLayout::grid;
      } else {
        return set_error(path + ".layout", "expected 'row', 'column', or 'grid'");
      }

      std::string style = "card";
      optional_string(value, "style", path, style);
      if (!ok()) {
        return false;
      }
      if (style == "card") {
        panel.style = PanelStyle::card;
      } else if (style == "bar") {
        panel.style = PanelStyle::bar;
      } else {
        return set_error(path + ".style", "expected 'card' or 'bar'");
      }

      const auto *bar = value.find("bar");
      if (bar != nullptr) {
        if (!bar->is_object()) {
          return set_error(path + ".bar", "expected object");
        }
        const auto width = optional_integer(*bar, "width", path + ".bar", 40, 1'200, panel.bar.width);
        const auto height = optional_integer(*bar, "height", path + ".bar", 16, 240, panel.bar.height);
        if (!width.has_value() || !height.has_value()) {
          return false;
        }
        panel.bar.width = static_cast<std::uint32_t>(*width);
        panel.bar.height = static_cast<std::uint32_t>(*height);
        optional_color(*bar, "fillColor", path + ".bar", panel.bar.fill_color);
        optional_color(*bar, "backgroundColor", path + ".bar", panel.bar.background_color);
        optional_color(*bar, "borderColor", path + ".bar", panel.bar.border_color);
        optional_color(*bar, "textColor", path + ".bar", panel.bar.text_color);
        optional_boolean(*bar, "showText", path + ".bar", panel.bar.show_text);
        optional_boolean(*bar, "hideWhenReady", path + ".bar", panel.bar.hide_when_ready);
        if (!ok()) {
          return false;
        }
      }

      const auto x = optional_number(value, "anchorX", path, 0.0, 1.0, 0.5);
      const auto y = optional_number(value, "anchorY", path, 0.0, 1.0, 0.5);
      if (!x.has_value() || !y.has_value()) {
        return false;
      }
      panel.anchor_x = *x;
      panel.anchor_y = *y;

      const auto *cooldowns = required_array(value, "cooldowns", path);
      if (cooldowns == nullptr) {
        return false;
      }
      if (cooldowns->empty()) {
        return set_error(path + ".cooldowns", "panel requires at least one cooldown id");
      }
      for (std::size_t item = 0; item < cooldowns->size(); ++item) {
        if (!(*cooldowns)[item].is_string()) {
          return set_error(path + ".cooldowns[" + std::to_string(item) + "]", "expected string cooldown id");
        }
        const auto &id = (*cooldowns)[item].as_string();
        if (!cooldown_ids_.contains(id)) {
          return set_error(path + ".cooldowns[" + std::to_string(item) + "]", "unknown cooldown id");
        }
        panel.cooldown_ids.push_back(id);
      }
      profile.panels.push_back(std::move(panel));
    }
    return true;
  }

bool ProfileReader::read_bindings(GameProfile &profile) {
    const auto *values = required_array(root_, "bindings", "$");
    if (values == nullptr) {
      return false;
    }
    if (values->empty()) {
      return set_error("$.bindings", "at least one binding is required");
    }

    profile.bindings.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
      const auto path = "$.bindings[" + std::to_string(index) + "]";
      const auto &value = (*values)[index];
      if (!value.is_object()) {
        return set_error(path, "binding must be an object");
      }

      InputBinding binding;
      if (!required_string(value, "cooldownId", path, binding.cooldown_id)) {
        return false;
      }
      if (!cooldown_ids_.contains(binding.cooldown_id)) {
        return set_error(path + ".cooldownId", "unknown cooldown id");
      }
      const auto *chord = required_array(value, "chord", path);
      if (chord == nullptr) {
        return false;
      }
      if (chord->empty()) {
        return set_error(path + ".chord", "chord requires at least one token");
      }

      std::set<InputToken> tokens;
      for (std::size_t token_index = 0; token_index < chord->size(); ++token_index) {
        const auto token_path = path + ".chord[" + std::to_string(token_index) + "]";
        const auto &token_value = (*chord)[token_index];
        if (!token_value.is_object()) {
          return set_error(token_path, "input token must be an object");
        }
        std::string device;
        if (!required_string(token_value, "device", token_path, device)) {
          return false;
        }
        InputToken token;
        if (device == "keyboard") {
          token.device = InputDevice::keyboard;
        } else if (device == "mouse") {
          token.device = InputDevice::mouse;
        } else if (device == "controller") {
          token.device = InputDevice::controller;
        } else if (device == "hid") {
          token.device = InputDevice::generic_hid;
        } else {
          return set_error(token_path + ".device",
                           "expected keyboard, mouse, controller, or hid");
        }
        if (token.device == InputDevice::generic_hid) {
          std::string binding_id;
          if (!required_string(token_value, "bindingId", token_path,
                               binding_id))
            return false;
          const auto found = hid_binding_ids_.find(binding_id);
          if (found == hid_binding_ids_.end())
            return set_error(token_path + ".bindingId",
                             "unknown HID binding id");
          token.code = found->second;
          if (!tokens.insert(token).second)
            return set_error(token_path, "duplicate token in chord");
          binding.chord.tokens.push_back(token);
          continue;
        }
        const std::int64_t maximum = token.device == InputDevice::mouse ? 7 :
                                     token.device == InputDevice::controller ? 18 : 65'535;
        const auto code = required_integer(token_value, "code", token_path, 1, maximum);
        const auto controller = optional_integer(token_value, "controllerIndex", token_path, 0, 3, 0);
        if (!code.has_value() || !controller.has_value()) {
          return false;
        }
        if (token.device == InputDevice::controller &&
            !((*code >= 1 && *code <= 10) ||
              (*code >= 13 && *code <= 18))) {
          return set_error(token_path + ".code",
                           "controller code must be 1-10 or 13-18");
        }
        token.code = static_cast<std::uint16_t>(*code);
        token.controller_index = static_cast<std::uint8_t>(*controller);
        if (token.device != InputDevice::controller && token.controller_index != 0) {
          return set_error(token_path + ".controllerIndex", "controllerIndex is valid only for controller tokens");
        }
        if (!tokens.insert(token).second) {
          return set_error(token_path, "duplicate token in chord");
        }
        binding.chord.tokens.push_back(token);
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
  auto parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed)) {
    return ProfileError{.path = "$@" + std::to_string(parse_error->offset),
                        .message = parse_error->message};
  }
  return detail::ProfileReader(std::get<json::Value>(parsed)).read();
}

} // namespace layerc::core
