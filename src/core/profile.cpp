#include "layerc/core/profile.hpp"

#include "profile_reader.hpp"
#include "utf8.hpp"

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
    const auto schema_version = required_integer(root_, "schemaVersion", "$", 1, 1);
    if (!schema_version.has_value()) {
      return error_;
    }
    profile.schema_version = static_cast<std::uint32_t>(*schema_version);

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
    if (!is_valid_utf8(profile.game.executable_path)) {
      return error("$.game.executablePath", "expected valid UTF-8");
    }
    if (!is_valid_utf8(profile.game.executable_name)) {
      return error("$.game.executableName", "expected valid UTF-8");
    }
    if (!is_valid_utf8(profile.game.window_class)) {
      return error("$.game.windowClass", "expected valid UTF-8");
    }
    if (!is_valid_utf8(profile.game.window_title)) {
      return error("$.game.windowTitle", "expected valid UTF-8");
    }
    if (!profile.game.global && profile.game.executable_path.empty() &&
        profile.game.executable_name.empty()) {
      return error("$.game", "executablePath or executableName is required");
    }

    if (!read_cooldowns(profile) || !read_panels(profile) ||
        !read_bindings(profile)) {
      return error_;
    }
    return profile;
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
        } else {
          return set_error(token_path + ".device", "expected keyboard, mouse, or controller");
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