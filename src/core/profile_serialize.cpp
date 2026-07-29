#include "layerc/core/profile.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace layerc::core {
namespace {

void append_unsigned(std::string &output, std::uint64_t value) {
  std::array<char, 32> digits{};
  auto *end = digits.data() + digits.size();
  auto *cursor = end;
  do {
    *--cursor = static_cast<char>('0' + value % 10U);
    value /= 10U;
  } while (value != 0);
  output.append(cursor, end);
}

void append_indent(std::string &output, const int count) {
  output.append(static_cast<std::size_t>(count), ' ');
}

void append_quoted(std::string &output, const std::string_view text) {
  output.push_back('"');
  constexpr std::string_view hex = "0123456789abcdef";
  for (const unsigned char character : text) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(hex[(character >> 4U) & 0x0FU]);
        output.push_back(hex[character & 0x0FU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

void append_key(std::string &output, const int indent,
                const std::string_view key) {
  append_indent(output, indent);
  append_quoted(output, key);
  output += ": ";
}

void append_color(std::string &output, const std::uint32_t color) {
  constexpr std::string_view hex = "0123456789ABCDEF";
  output.push_back('"');
  output.push_back('#');
  for (int shift = 20; shift >= 0; shift -= 4) {
    output.push_back(hex[(color >> static_cast<unsigned>(shift)) & 0xFU]);
  }
  output.push_back('"');
}

std::string_view device_name(const InputDevice device) {
  switch (device) {
  case InputDevice::keyboard:
    return "keyboard";
  case InputDevice::mouse:
    return "mouse";
  case InputDevice::controller:
    return "controller";
  }
  return "keyboard";
}

std::string_view retrigger_name(const RetriggerPolicy policy) {
  return policy == RetriggerPolicy::restart ? "restart" : "ignore";
}

std::string_view layout_name(const PanelLayout layout) {
  switch (layout) {
  case PanelLayout::row:
    return "row";
  case PanelLayout::column:
    return "column";
  case PanelLayout::grid:
    return "grid";
  }
  return "row";
}

std::string_view style_name(const PanelStyle style) {
  return style == PanelStyle::bar ? "bar" : "card";
}

void append_bool(std::string &output, const bool value) {
  output += value ? "true" : "false";
}

} // namespace

std::string serialize_profile_json(const GameProfile &profile) {
  std::string output;
  output.reserve(2048);
  output += "{\n";
  append_key(output, 2, "schemaVersion");
  append_unsigned(output, profile.schema_version);
  output += ",\n";
  append_key(output, 2, "id");
  append_quoted(output, profile.id);
  output += ",\n";
  append_key(output, 2, "name");
  append_quoted(output, profile.name);
  output += ",\n";  append_key(output, 2, "enabled");
  append_bool(output, profile.enabled);
  output += ",\n";

  append_key(output, 2, "game");
  output += "{\n";
  append_key(output, 4, "global");
  append_bool(output, profile.game.global);
  output += ",\n";
  append_key(output, 4, "executablePath");
  append_quoted(output, profile.game.executable_path);
  output += ",\n";
  append_key(output, 4, "executableName");
  append_quoted(output, profile.game.executable_name);
  output += ",\n";
  append_key(output, 4, "windowClass");
  append_quoted(output, profile.game.window_class);
  output += ",\n";
  append_key(output, 4, "windowTitle");
  append_quoted(output, profile.game.window_title);
  output += "\n  },\n";

  append_key(output, 2, "cooldowns");
  output += "[\n";
  for (std::size_t index = 0; index < profile.cooldowns.size(); ++index) {
    const auto &cooldown = profile.cooldowns[index];
    append_indent(output, 4);
    output += "{\n";
    append_key(output, 6, "id");
    append_quoted(output, cooldown.id);
    output += ",\n";
    append_key(output, 6, "name");
    append_quoted(output, cooldown.name);
    output += ",\n";
    append_key(output, 6, "durationMs");
    append_unsigned(output,
                    static_cast<std::uint64_t>(cooldown.duration.count()));
    output += ",\n";
    append_key(output, 6, "maxCharges");
    append_unsigned(output, cooldown.max_charges);
    output += ",\n";
    append_key(output, 6, "retrigger");
    append_quoted(output, retrigger_name(cooldown.retrigger));
    output += ",\n";
    append_key(output, 6, "stackCharges");
    append_bool(output, cooldown.stack_charges);
    output += "\n    }";
    output += index + 1 == profile.cooldowns.size() ? "\n" : ",\n";
  }
  output += "  ],\n";

  append_key(output, 2, "panels");
  output += "[\n";
  for (std::size_t index = 0; index < profile.panels.size(); ++index) {
    const auto &panel = profile.panels[index];
    append_indent(output, 4);
    output += "{\n";
    append_key(output, 6, "id");
    append_quoted(output, panel.id);
    output += ",\n";
    append_key(output, 6, "name");
    append_quoted(output, panel.name);
    output += ",\n";
    append_key(output, 6, "layout");
    append_quoted(output, layout_name(panel.layout));
    output += ",\n";
    append_key(output, 6, "style");
    append_quoted(output, style_name(panel.style));
    output += ",\n";
    append_key(output, 6, "bar");
    output += "{\n";
    append_key(output, 8, "width");
    append_unsigned(output, panel.bar.width);
    output += ",\n";
    append_key(output, 8, "height");
    append_unsigned(output, panel.bar.height);
    output += ",\n";
    append_key(output, 8, "fillColor");
    append_color(output, panel.bar.fill_color);
    output += ",\n";
    append_key(output, 8, "backgroundColor");
    append_color(output, panel.bar.background_color);
    output += ",\n";
    append_key(output, 8, "borderColor");
    append_color(output, panel.bar.border_color);
    output += ",\n";
    append_key(output, 8, "textColor");
    append_color(output, panel.bar.text_color);
    output += ",\n";
    append_key(output, 8, "hideWhenReady");
    append_bool(output, panel.bar.hide_when_ready);
    output += "\n      },\n";
    append_key(output, 6, "anchorX");
    output += std::to_string(panel.anchor_x);
    output += ",\n";
    append_key(output, 6, "anchorY");
    output += std::to_string(panel.anchor_y);
    output += ",\n";
    append_key(output, 6, "cooldowns");
    output += "[";
    for (std::size_t cooldown_index = 0;
         cooldown_index < panel.cooldown_ids.size(); ++cooldown_index) {
      if (cooldown_index != 0) {
        output += ", ";
      }
      append_quoted(output, panel.cooldown_ids[cooldown_index]);
    }
    output += "]\n    }";
    output += index + 1 == profile.panels.size() ? "\n" : ",\n";
  }
  output += "  ],\n";

  append_key(output, 2, "bindings");
  output += "[\n";
  for (std::size_t index = 0; index < profile.bindings.size(); ++index) {
    const auto &binding = profile.bindings[index];
    append_indent(output, 4);
    output += "{\n";
    append_key(output, 6, "cooldownId");
    append_quoted(output, binding.cooldown_id);
    output += ",\n";
    append_key(output, 6, "chord");
    output += "[\n";
    for (std::size_t token_index = 0; token_index < binding.chord.tokens.size();
         ++token_index) {
      const auto &token = binding.chord.tokens[token_index];
      append_indent(output, 8);
      output += "{\n";
      append_key(output, 10, "device");
      append_quoted(output, device_name(token.device));
      output += ",\n";
      append_key(output, 10, "code");
      append_unsigned(output, token.code);
      if (token.device == InputDevice::controller) {
        output += ",\n";
        append_key(output, 10, "controllerIndex");
        append_unsigned(output, token.controller_index);
      }
      output += "\n        }";
      output += token_index + 1 == binding.chord.tokens.size() ? "\n" : ",\n";
    }
    output += "      ]\n    }";
    output += index + 1 == profile.bindings.size() ? "\n" : ",\n";
  }
  output += "  ]\n}\n";
  return output;
}

} // namespace layerc::core