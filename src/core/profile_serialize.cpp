#include "layerc/core/profile.hpp"
#include "layerc/core/json.hpp"

#include <cstdint>
#include <string_view>

namespace layerc::core {
namespace {

void append_indent(std::string &output, const int count) {
  output.append(static_cast<std::size_t>(count), ' ');
}

void append_key(std::string &output, const int indent,
                const std::string_view key) {
  append_indent(output, indent);
  json::append_quoted(output, key);
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
  case InputDevice::generic_hid:
    return "hid";
  }
  return "keyboard";
}

std::string_view retrigger_name(const RetriggerPolicy policy) {
  return policy == RetriggerPolicy::restart ? "restart" : "ignore";
}
std::string_view recovery_name(const RecoveryMode mode) {
  switch (mode) {
  case RecoveryMode::single:
    return "single";
  case RecoveryMode::sequential:
    return "sequential";
  case RecoveryMode::parallel:
    return "parallel";
  }
  return "single";
}

std::string_view group_mode_name(const CooldownGroupMode mode) {
  return mode == CooldownGroupMode::shared_pool ? "sharedPool"
                                                : "triggerPropagation";
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

std::string_view hid_activation_name(const HidActivationMode activation) {
  switch (activation) {
  case HidActivationMode::button:
    return "button";
  case HidActivationMode::equals:
    return "equals";
  case HidActivationMode::axis_positive:
    return "axisPositive";
  case HidActivationMode::axis_negative:
    return "axisNegative";
  case HidActivationMode::relative_positive:
    return "relativePositive";
  case HidActivationMode::relative_negative:
    return "relativeNegative";
  }
  return "button";
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
  json::append_unsigned(output, 4);
  output += ",\n";
  append_key(output, 2, "id");
  json::append_quoted(output, profile.id);
  output += ",\n";
  append_key(output, 2, "name");
  json::append_quoted(output, profile.name);
  output += ",\n";  append_key(output, 2, "enabled");
  append_bool(output, profile.enabled);
  output += ",\n";

  append_key(output, 2, "game");
  output += "{\n";
  append_key(output, 4, "global");
  append_bool(output, profile.game.global);
  output += ",\n";
  append_key(output, 4, "executablePath");
  json::append_quoted(output, profile.game.executable_path);
  output += ",\n";
  append_key(output, 4, "executableName");
  json::append_quoted(output, profile.game.executable_name);
  output += ",\n";
  append_key(output, 4, "windowClass");
  json::append_quoted(output, profile.game.window_class);
  output += ",\n";
  append_key(output, 4, "windowTitle");
  json::append_quoted(output, profile.game.window_title);
  output += "\n  },\n";

  append_key(output, 2, "visualRuntime");
  json::append_quoted(output, profile.visual_runtime == VisualRuntime::isolated
                                 ? "isolated"
                                 : "inline");
  output += ",\n";
  append_key(output, 2, "modules");
  output += "[";
  for (std::size_t index = 0; index < profile.modules.size(); ++index) {
    if (index != 0) output += ", ";
    output += "{\"kind\":\"";
    output += profile.modules[index].kind == ProfileModuleKind::automation
                  ? "automation"
                  : "visual";
    output += "\",\"schemaVersion\":";
    json::append_unsigned(output, profile.modules[index].schema_version);
    output += "}";
  }
  output += "],\n";

  append_key(output, 2, "cooldowns");
  output += "[\n";
  for (std::size_t index = 0; index < profile.cooldowns.size(); ++index) {
    const auto &cooldown = profile.cooldowns[index];
    append_indent(output, 4);
    output += "{\n";
    append_key(output, 6, "id");
    json::append_quoted(output, cooldown.id);
    output += ",\n";
    append_key(output, 6, "name");
    json::append_quoted(output, cooldown.name);
    output += ",\n";
    append_key(output, 6, "durationMs");
    json::append_unsigned(
        output, static_cast<std::uint64_t>(cooldown.duration.count()));
    output += ",\n";
    append_key(output, 6, "maxCharges");
    json::append_unsigned(output, cooldown.max_charges);
    output += ",\n";
    append_key(output, 6, "retrigger");
    json::append_quoted(output, retrigger_name(cooldown.retrigger));
    output += ",\n";
    append_key(output, 6, "recovery");
    json::append_quoted(output, recovery_name(cooldown.recovery));
    output += "\n    }";
    output += index + 1 == profile.cooldowns.size() ? "\n" : ",\n";
  }
  output += "  ],\n";

  append_key(output, 2, "cooldownGroups");
  output += "[\n";
  for (std::size_t index{}; index < profile.cooldown_groups.size(); ++index) {
    const auto &group = profile.cooldown_groups[index];
    output += "    {\n";
    append_key(output, 6, "id");
    json::append_quoted(output, group.id);
    output += ",\n";
    append_key(output, 6, "name");
    json::append_quoted(output, group.name);
    output += ",\n";
    append_key(output, 6, "mode");
    json::append_quoted(output, group_mode_name(group.mode));
    output += ",\n";
    append_key(output, 6, "cooldowns");
    output += "[";
    for (std::size_t member{}; member < group.cooldown_ids.size(); ++member) {
      if (member != 0) output += ", ";
      json::append_quoted(output, group.cooldown_ids[member]);
    }
    output += "]";
    if (group.mode == CooldownGroupMode::shared_pool) {
      output += ",\n";
      append_key(output, 6, "durationMs");
      json::append_unsigned(
          output, static_cast<std::uint64_t>(group.duration.count()));
      output += ",\n";
      append_key(output, 6, "maxCharges");
      json::append_unsigned(output, group.max_charges);
      output += ",\n";
      append_key(output, 6, "recovery");
      json::append_quoted(output, recovery_name(group.recovery));
      output += ",\n";
      append_key(output, 6, "retrigger");
      json::append_quoted(output, retrigger_name(group.retrigger));
    }
    output += "\n    }";
    output += index + 1U == profile.cooldown_groups.size() ? "\n" : ",\n";
  }
  output += "  ],\n";
  append_key(output, 2, "hidBindings");
  output += "[\n";
  for (std::size_t index = 0; index < profile.hid_bindings.size(); ++index) {
    const auto &binding = profile.hid_bindings[index];
    output += "    {\n";
    append_key(output, 6, "id");
    json::append_quoted(output, binding.id);
    output += ",\n";
    append_key(output, 6, "device");
    output += "{\"vid\":";
    json::append_unsigned(output, binding.device.vendor_id);
    output += ",\"pid\":";
    json::append_unsigned(output, binding.device.product_id);
    output += ",\"version\":";
    json::append_unsigned(output, binding.device.version);
    output += ",\"usagePage\":";
    json::append_unsigned(output, binding.device.usage_page);
    output += ",\"usage\":";
    json::append_unsigned(output, binding.device.usage);
    output += ",\"descriptorHash\":";
    json::append_quoted(output, binding.device.descriptor_hash);
    if (!binding.device.serial.empty()) {
      output += ",\"serial\":";
      json::append_quoted(output, binding.device.serial);
    }
    output += ",\"stableSlot\":";
    json::append_unsigned(output, binding.device.stable_slot);
    output += "},\n";
    append_key(output, 6, "control");
    output += "{\"reportId\":";
    json::append_unsigned(output, binding.report_id);
    output += ",\"usagePage\":";
    json::append_unsigned(output, binding.usage_page);
    output += ",\"usage\":";
    json::append_unsigned(output, binding.usage);
    output += ",\"linkCollection\":";
    json::append_unsigned(output, binding.link_collection);
    output += ",\"activation\":";
    json::append_quoted(output, hid_activation_name(binding.activation));
    output += ",\"value\":";
    json::append_number(output, binding.discrete_value);
    output += ",\"threshold\":";
    json::append_number(output, binding.threshold);
    output += ",\"hysteresis\":";
    json::append_number(output, binding.hysteresis);
    output += "},\n";
    append_key(output, 6, "allowSystemClass");
    append_bool(output, binding.allow_system_class);
    output += "\n    }";
    output += index + 1U == profile.hid_bindings.size() ? "\n" : ",\n";
  }
  output += "  ],\n";

  append_key(output, 2, "panels");
  output += "[\n";
  for (std::size_t index = 0; index < profile.panels.size(); ++index) {
    const auto &panel = profile.panels[index];
    append_indent(output, 4);
    output += "{\n";
    append_key(output, 6, "id");
    json::append_quoted(output, panel.id);
    output += ",\n";
    append_key(output, 6, "name");
    json::append_quoted(output, panel.name);
    output += ",\n";
    append_key(output, 6, "layout");
    json::append_quoted(output, layout_name(panel.layout));
    output += ",\n";
    append_key(output, 6, "style");
    json::append_quoted(output, style_name(panel.style));
    output += ",\n";
    append_key(output, 6, "bar");
    output += "{\n";
    append_key(output, 8, "width");
    json::append_unsigned(output, panel.bar.width);
    output += ",\n";
    append_key(output, 8, "height");
    json::append_unsigned(output, panel.bar.height);
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
    append_key(output, 8, "showText");
    append_bool(output, panel.bar.show_text);
    output += ",\n";
    append_key(output, 8, "hideWhenReady");
    append_bool(output, panel.bar.hide_when_ready);
    output += "\n      },\n";
    append_key(output, 6, "anchorX");
    json::append_number(output, panel.anchor_x);
    output += ",\n";
    append_key(output, 6, "anchorY");
    json::append_number(output, panel.anchor_y);
    output += ",\n";
    append_key(output, 6, "cooldowns");
    output += "[";
    for (std::size_t cooldown_index = 0;
         cooldown_index < panel.cooldown_ids.size(); ++cooldown_index) {
      if (cooldown_index != 0) {
        output += ", ";
      }
      json::append_quoted(output, panel.cooldown_ids[cooldown_index]);
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
    json::append_quoted(output, binding.cooldown_id);
    output += ",\n";
    append_key(output, 6, "chord");
    output += "[\n";
    for (std::size_t token_index = 0; token_index < binding.chord.tokens.size();
         ++token_index) {
      const auto &token = binding.chord.tokens[token_index];
      append_indent(output, 8);
      output += "{\n";
      append_key(output, 10, "device");
      json::append_quoted(output, device_name(token.device));
      if (token.device == InputDevice::generic_hid) {
        output += ",\n";
        append_key(output, 10, "bindingId");
        if (token.code > 0 &&
            token.code <= profile.hid_bindings.size())
          json::append_quoted(output,
                              profile.hid_bindings[token.code - 1U].id);
        else
          json::append_quoted(output, "");
      } else {
        output += ",\n";
        append_key(output, 10, "code");
        json::append_unsigned(output, token.code);
        if (token.device == InputDevice::controller) {
          output += ",\n";
          append_key(output, 10, "controllerIndex");
          json::append_unsigned(output, token.controller_index);
        }
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
