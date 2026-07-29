#include "layerc/core/profile.hpp"

#include "layerc/core/json.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace layerc::core {
namespace {

std::optional<std::uint32_t> parse_hex_color(const std::string_view value) {
  if (value.size() != 7 || value.front() != '#') {
    return std::nullopt;
  }

  std::uint32_t color{};
  for (std::size_t index = 1; index < value.size(); ++index) {
    const auto character = value[index];
    std::uint32_t digit{};
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint32_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint32_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint32_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    color = (color << 4U) | digit;
  }
  return color;
}

bool is_valid_utf8(const std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuations{};
    std::uint32_t codepoint{};
    std::uint32_t minimum{};
    if ((first & 0xE0U) == 0xC0U) {
      continuations = 1;
      codepoint = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuations = 2;
      codepoint = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuations = 3;
      codepoint = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (index + continuations >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuations; ++offset) {
      const auto next = static_cast<std::uint8_t>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += continuations + 1;
  }
  return true;
}

class ProfileReader {
public:
  explicit ProfileReader(const json::Value &root) : root_(root) {}

  ProfileResult read() {
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

private:
  [[nodiscard]] bool ok() const { return error_.message.empty(); }

  bool read_cooldowns(GameProfile &profile) {
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

  bool read_panels(GameProfile &profile) {
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

  bool read_bindings(GameProfile &profile) {
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

  const json::Value *required_object(const json::Value &parent,
                                     const std::string_view key,
                                     const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_object()) {
      set_error(path + "." + std::string(key), "expected object");
      return nullptr;
    }
    return value;
  }

  const json::Value::Array *required_array(const json::Value &parent,
                                            const std::string_view key,
                                            const std::string &path) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return nullptr;
    }
    if (!value->is_array()) {
      set_error(path + "." + std::string(key), "expected array");
      return nullptr;
    }
    return &value->as_array();
  }

  bool required_string(const json::Value &parent, const std::string_view key,
                       const std::string &path, std::string &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return set_error(path + "." + std::string(key), "required value is missing");
    }
    if (!value->is_string() || value->as_string().empty()) {
      return set_error(path + "." + std::string(key), "expected non-empty string");
    }
    output = value->as_string();
    return true;
  }

  void optional_string(const json::Value &parent, const std::string_view key,
                       const std::string &path, std::string &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_string()) {
      set_error(path + "." + std::string(key), "expected string");
      return;
    }
    output = value->as_string();
  }

  void optional_boolean(const json::Value &parent, const std::string_view key,
                        const std::string &path, bool &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_bool()) {
      set_error(path + "." + std::string(key), "expected boolean");
      return;
    }
    output = value->as_bool();
  }

  void optional_color(const json::Value &parent, const std::string_view key,
                      const std::string &path, std::uint32_t &output) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return;
    }
    if (!value->is_string()) {
      set_error(path + "." + std::string(key), "expected #RRGGBB color string");
      return;
    }
    const auto parsed = parse_hex_color(value->as_string());
    if (!parsed.has_value()) {
      set_error(path + "." + std::string(key), "expected #RRGGBB color string");
      return;
    }
    output = *parsed;
  }

  std::optional<std::int64_t> required_integer(const json::Value &parent,
                                                const std::string_view key,
                                                const std::string &path,
                                                const std::int64_t minimum,
                                                const std::int64_t maximum) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      set_error(path + "." + std::string(key), "required value is missing");
      return std::nullopt;
    }
    return checked_integer(*value, path + "." + std::string(key), minimum, maximum);
  }

  std::optional<std::int64_t> optional_integer(const json::Value &parent,
                                                const std::string_view key,
                                                const std::string &path,
                                                const std::int64_t minimum,
                                                const std::int64_t maximum,
                                                const std::int64_t fallback) {
    const auto *value = parent.find(key);
    return value == nullptr ? std::optional<std::int64_t>(fallback) :
        checked_integer(*value, path + "." + std::string(key), minimum, maximum);
  }

  std::optional<std::int64_t> checked_integer(const json::Value &value,
                                               const std::string &path,
                                               const std::int64_t minimum,
                                               const std::int64_t maximum) {
    if (!value.is_number()) {
      set_error(path, "expected integer");
      return std::nullopt;
    }
    const auto number = value.as_number();
    if (std::floor(number) != number || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum)) {
      set_error(path, "integer is outside the supported range");
      return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
  }

  std::optional<double> optional_number(const json::Value &parent,
                                        const std::string_view key,
                                        const std::string &path,
                                        const double minimum,
                                        const double maximum,
                                        const double fallback) {
    const auto *value = parent.find(key);
    if (value == nullptr) {
      return fallback;
    }
    if (!value->is_number() || value->as_number() < minimum ||
        value->as_number() > maximum) {
      set_error(path + "." + std::string(key), "number is outside the supported range");
      return std::nullopt;
    }
    return value->as_number();
  }

  ProfileError error(const std::string &path, const std::string &message) {
    return ProfileError{.path = path, .message = message};
  }

  bool set_error(std::string path, std::string message) {
    if (error_.message.empty()) {
      error_ = ProfileError{.path = std::move(path), .message = std::move(message)};
    }
    return false;
  }

  const json::Value &root_;
  std::set<std::string, std::less<>> cooldown_ids_;
  ProfileError error_;
};

} // namespace

ProfileResult parse_profile_json(const std::string_view source) {
  if (source.size() > max_profile_json_bytes) {
    return ProfileError{.path = "$", .message = "profile exceeds the 256 KiB limit"};
  }
  auto parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed)) {
    return ProfileError{.path = "$@" + std::to_string(parse_error->offset),
                        .message = parse_error->message};
  }
  return ProfileReader(std::get<json::Value>(parsed)).read();
}

} // namespace layerc::core