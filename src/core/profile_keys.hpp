#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace layerc::core::profile_keys {

struct FieldDescriptor {
  std::string_view name;
  bool recognized_legacy_only{};
};

template <std::size_t N>
consteval std::array<std::string_view, N>
field_names(const std::array<FieldDescriptor, N> &fields) {
  std::array<std::string_view, N> names{};
  for (std::size_t i{}; i < N; ++i) names[i] = fields[i].name;
  return names;
}

// A field is declared once, including its owning object and legacy policy.
// Reader/serializer constants and extension classification are generated from
// that declaration; adding a consumed field needs no third inventory edit.
#define LAYERC_PROFILE_ROOT_FIELDS(X) \
  X(schema_version, "schemaVersion", false) \
  X(id, "id", false) \
  X(name, "name", false) \
  X(enabled, "enabled", false) \
  X(game, "game", false) \
  X(visual_runtime, "visualRuntime", false) \
  X(modules, "modules", false) \
  X(cooldowns, "cooldowns", false) \
  X(cooldown_groups, "cooldownGroups", false) \
  X(hid_bindings, "hidBindings", false) \
  X(panels, "panels", false) \
  X(bindings, "bindings", false)
#define LAYERC_PROFILE_GAME_FIELDS(X) \
  X(global, "global", false) \
  X(executable_path, "executablePath", false) \
  X(executable_name, "executableName", false) \
  X(window_class, "windowClass", false) \
  X(window_title, "windowTitle", false)
#define LAYERC_PROFILE_MODULE_FIELDS(X) \
  X(kind, "kind", false) \
  X(schema_version, "schemaVersion", false)
#define LAYERC_PROFILE_COOLDOWN_FIELDS(X) \
  X(id, "id", false) \
  X(name, "name", false) \
  X(duration_ms, "durationMs", false) \
  X(max_charges, "maxCharges", false) \
  X(retrigger, "retrigger", false) \
  X(recovery, "recovery", false) \
  X(stack_charges, "stackCharges", true)
#define LAYERC_PROFILE_GROUP_FIELDS(X) \
  X(id, "id", false) \
  X(name, "name", false) \
  X(mode, "mode", false) \
  X(cooldowns, "cooldowns", false) \
  X(duration_ms, "durationMs", false) \
  X(max_charges, "maxCharges", false) \
  X(retrigger, "retrigger", false) \
  X(recovery, "recovery", false)
#define LAYERC_PROFILE_HID_FIELDS(X) \
  X(id, "id", false) \
  X(device, "device", false) \
  X(control, "control", false) \
  X(allow_system_class, "allowSystemClass", false)
#define LAYERC_PROFILE_DEVICE_FIELDS(X) \
  X(vid, "vid", false) \
  X(pid, "pid", false) \
  X(version, "version", false) \
  X(usage_page, "usagePage", false) \
  X(usage, "usage", false) \
  X(descriptor_hash, "descriptorHash", false) \
  X(stable_slot, "stableSlot", false) \
  X(serial, "serial", false)
#define LAYERC_PROFILE_CONTROL_FIELDS(X) \
  X(report_id, "reportId", false) \
  X(usage_page, "usagePage", false) \
  X(usage, "usage", false) \
  X(link_collection, "linkCollection", false) \
  X(activation, "activation", false) \
  X(value, "value", false) \
  X(threshold, "threshold", false) \
  X(hysteresis, "hysteresis", false)
#define LAYERC_PROFILE_PANEL_FIELDS(X) \
  X(id, "id", false) \
  X(name, "name", false) \
  X(layout, "layout", false) \
  X(style, "style", false) \
  X(bar, "bar", false) \
  X(anchor_x, "anchorX", false) \
  X(anchor_y, "anchorY", false) \
  X(cooldowns, "cooldowns", false)
#define LAYERC_PROFILE_BAR_FIELDS(X) \
  X(width, "width", false) \
  X(height, "height", false) \
  X(fill_color, "fillColor", false) \
  X(background_color, "backgroundColor", false) \
  X(border_color, "borderColor", false) \
  X(text_color, "textColor", false) \
  X(show_text, "showText", false) \
  X(hide_when_ready, "hideWhenReady", false)
#define LAYERC_PROFILE_BINDING_FIELDS(X) \
  X(cooldown_id, "cooldownId", false) \
  X(chord, "chord", false)
#define LAYERC_PROFILE_TOKEN_FIELDS(X) \
  X(device, "device", false) \
  X(code, "code", false) \
  X(controller_index, "controllerIndex", false) \
  X(binding_id, "bindingId", false)

#define LAYERC_PROFILE_DECLARE_FIELD(identifier, wire, legacy) \
  inline constexpr std::string_view identifier = wire;
#define LAYERC_PROFILE_DESCRIBE_FIELD(identifier, wire, legacy) \
  FieldDescriptor{identifier, legacy},
#define LAYERC_PROFILE_DECLARE_OBJECT(scope, fields_macro) \
  namespace scope { \
    fields_macro(LAYERC_PROFILE_DECLARE_FIELD) \
    inline constexpr std::array fields{ \
        fields_macro(LAYERC_PROFILE_DESCRIBE_FIELD)}; \
    inline constexpr auto names = field_names(fields); \
  }

LAYERC_PROFILE_DECLARE_OBJECT(root, LAYERC_PROFILE_ROOT_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(game, LAYERC_PROFILE_GAME_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(module, LAYERC_PROFILE_MODULE_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(cooldown, LAYERC_PROFILE_COOLDOWN_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(group, LAYERC_PROFILE_GROUP_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(hid, LAYERC_PROFILE_HID_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(device, LAYERC_PROFILE_DEVICE_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(control, LAYERC_PROFILE_CONTROL_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(panel, LAYERC_PROFILE_PANEL_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(bar, LAYERC_PROFILE_BAR_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(binding, LAYERC_PROFILE_BINDING_FIELDS)
LAYERC_PROFILE_DECLARE_OBJECT(token, LAYERC_PROFILE_TOKEN_FIELDS)

#undef LAYERC_PROFILE_DECLARE_OBJECT
#undef LAYERC_PROFILE_DESCRIBE_FIELD
#undef LAYERC_PROFILE_DECLARE_FIELD
#undef LAYERC_PROFILE_TOKEN_FIELDS
#undef LAYERC_PROFILE_BINDING_FIELDS
#undef LAYERC_PROFILE_BAR_FIELDS
#undef LAYERC_PROFILE_PANEL_FIELDS
#undef LAYERC_PROFILE_CONTROL_FIELDS
#undef LAYERC_PROFILE_DEVICE_FIELDS
#undef LAYERC_PROFILE_HID_FIELDS
#undef LAYERC_PROFILE_GROUP_FIELDS
#undef LAYERC_PROFILE_COOLDOWN_FIELDS
#undef LAYERC_PROFILE_MODULE_FIELDS
#undef LAYERC_PROFILE_GAME_FIELDS
#undef LAYERC_PROFILE_ROOT_FIELDS

} // namespace layerc::core::profile_keys
