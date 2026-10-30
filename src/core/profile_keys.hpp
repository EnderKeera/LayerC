#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace layerc::core::profile_keys {

struct FieldDescriptor {
  std::string_view name;
  bool recognized_legacy_only{};
  std::uint32_t since_schema{1};
};

template <std::size_t N>
consteval std::array<std::string_view, N>
field_names(const std::array<FieldDescriptor, N> &fields) {
  std::array<std::string_view, N> names{};
  for (std::size_t i{}; i < N; ++i) names[i] = fields[i].name;
  return names;
}

template <std::size_t N> struct FilteredFieldNames {
  std::array<std::string_view, N> values{};
  std::size_t size{};

  [[nodiscard]] constexpr std::span<const std::string_view> view() const noexcept {
    return {values.data(), size};
  }
};

template <std::uint32_t Schema, std::size_t N>
consteval FilteredFieldNames<N>
filtered_field_names(const std::array<FieldDescriptor, N> &fields) {
  FilteredFieldNames<N> result;
  for (const auto &field : fields)
    if (field.since_schema <= Schema)
      result.values[result.size++] = field.name;
  return result;
}

// A field is declared once, including its owning object and schema policy.
// Reader/serializer constants and extension classification are generated from
// that declaration; adding a consumed field needs no third inventory edit.
#define LAYERC_PROFILE_ROOT_FIELDS(X) \
  X(schema_version, "schemaVersion", false, 1) \
  X(id, "id", false, 1) \
  X(name, "name", false, 1) \
  X(enabled, "enabled", false, 1) \
  X(game, "game", false, 1) \
  X(visual_runtime, "visualRuntime", false, 1) \
  X(modules, "modules", false, 1) \
  X(cooldowns, "cooldowns", false, 1) \
  X(cooldown_groups, "cooldownGroups", false, 1) \
  X(hid_bindings, "hidBindings", false, 1) \
  X(panels, "panels", false, 1) \
  X(bindings, "bindings", false, 1)
#define LAYERC_PROFILE_GAME_FIELDS(X) \
  X(global, "global", false, 1) \
  X(executable_path, "executablePath", false, 1) \
  X(executable_name, "executableName", false, 1) \
  X(window_class, "windowClass", false, 1) \
  X(window_title, "windowTitle", false, 1)
#define LAYERC_PROFILE_MODULE_FIELDS(X) \
  X(kind, "kind", false, 1) \
  X(schema_version, "schemaVersion", false, 1)
#define LAYERC_PROFILE_COOLDOWN_FIELDS(X) \
  X(id, "id", false, 1) \
  X(name, "name", false, 1) \
  X(duration_ms, "durationMs", false, 1) \
  X(max_charges, "maxCharges", false, 1) \
  X(retrigger, "retrigger", false, 1) \
  X(recovery, "recovery", false, 1) \
  X(stack_charges, "stackCharges", true, 1) \
  X(active_duration_ms, "activeDurationMs", false, 5)
#define LAYERC_PROFILE_GROUP_FIELDS(X) \
  X(id, "id", false, 1) \
  X(name, "name", false, 1) \
  X(mode, "mode", false, 1) \
  X(cooldowns, "cooldowns", false, 1) \
  X(duration_ms, "durationMs", false, 1) \
  X(max_charges, "maxCharges", false, 1) \
  X(retrigger, "retrigger", false, 1) \
  X(recovery, "recovery", false, 1)
#define LAYERC_PROFILE_HID_FIELDS(X) \
  X(id, "id", false, 1) \
  X(device, "device", false, 1) \
  X(control, "control", false, 1) \
  X(allow_system_class, "allowSystemClass", false, 1)
#define LAYERC_PROFILE_DEVICE_FIELDS(X) \
  X(vid, "vid", false, 1) \
  X(pid, "pid", false, 1) \
  X(version, "version", false, 1) \
  X(usage_page, "usagePage", false, 1) \
  X(usage, "usage", false, 1) \
  X(descriptor_hash, "descriptorHash", false, 1) \
  X(stable_slot, "stableSlot", false, 1) \
  X(serial, "serial", false, 1)
#define LAYERC_PROFILE_CONTROL_FIELDS(X) \
  X(report_id, "reportId", false, 1) \
  X(usage_page, "usagePage", false, 1) \
  X(usage, "usage", false, 1) \
  X(link_collection, "linkCollection", false, 1) \
  X(activation, "activation", false, 1) \
  X(value, "value", false, 1) \
  X(threshold, "threshold", false, 1) \
  X(hysteresis, "hysteresis", false, 1)
#define LAYERC_PROFILE_PANEL_FIELDS(X) \
  X(id, "id", false, 1) \
  X(name, "name", false, 1) \
  X(layout, "layout", false, 1) \
  X(style, "style", false, 1) \
  X(bar, "bar", false, 1) \
  X(anchor_x, "anchorX", false, 1) \
  X(anchor_y, "anchorY", false, 1) \
  X(cooldowns, "cooldowns", false, 1) \
  X(items, "items", false, 5)
#define LAYERC_PROFILE_BAR_FIELDS(X) \
  X(width, "width", false, 1) \
  X(height, "height", false, 1) \
  X(fill_color, "fillColor", false, 1) \
  X(background_color, "backgroundColor", false, 1) \
  X(border_color, "borderColor", false, 1) \
  X(text_color, "textColor", false, 1) \
  X(show_text, "showText", false, 1) \
  X(hide_when_ready, "hideWhenReady", false, 1)
#define LAYERC_PROFILE_BINDING_FIELDS(X) \
  X(cooldown_id, "cooldownId", false, 1) \
  X(chord, "chord", false, 1)
#define LAYERC_PROFILE_TOKEN_FIELDS(X) \
  X(device, "device", false, 1) \
  X(code, "code", false, 1) \
  X(controller_index, "controllerIndex", false, 1) \
  X(binding_id, "bindingId", false, 1)
#define LAYERC_PROFILE_ITEM_FIELDS(X) \
  X(id, "id", false, 5) \
  X(kind, "kind", false, 5) \
  X(label, "label", false, 5) \
  X(cooldown_id, "cooldownId", false, 5) \
  X(source, "source", false, 5) \
  X(timer_id, "timerId", false, 5) \
  X(variable, "variable", false, 5) \
  X(max, "max", false, 5) \
  X(display, "display", false, 5) \
  X(flash_hz, "flashHz", false, 5)

#define LAYERC_PROFILE_DECLARE_FIELD(identifier, wire, legacy, since) \
  inline constexpr std::string_view identifier = wire;
#define LAYERC_PROFILE_DESCRIBE_FIELD(identifier, wire, legacy, since) \
  FieldDescriptor{identifier, legacy, since},
#define LAYERC_PROFILE_DECLARE_OBJECT(scope, fields_macro) \
  namespace scope { \
    fields_macro(LAYERC_PROFILE_DECLARE_FIELD) \
    inline constexpr std::array fields{ \
        fields_macro(LAYERC_PROFILE_DESCRIBE_FIELD)}; \
    inline constexpr auto names = field_names(fields); \
    inline constexpr auto schema4_names = filtered_field_names<4>(fields); \
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
LAYERC_PROFILE_DECLARE_OBJECT(item, LAYERC_PROFILE_ITEM_FIELDS)

#undef LAYERC_PROFILE_DECLARE_OBJECT
#undef LAYERC_PROFILE_DESCRIBE_FIELD
#undef LAYERC_PROFILE_DECLARE_FIELD
#undef LAYERC_PROFILE_ITEM_FIELDS
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
