#pragma once

#include <string_view>

namespace layerc::core::profile_keys {

inline constexpr std::string_view schema_version = "schemaVersion";
inline constexpr std::string_view id = "id";
inline constexpr std::string_view name = "name";
inline constexpr std::string_view enabled = "enabled";
inline constexpr std::string_view game = "game";
inline constexpr std::string_view global = "global";
inline constexpr std::string_view executable_path = "executablePath";
inline constexpr std::string_view executable_name = "executableName";
inline constexpr std::string_view window_class = "windowClass";
inline constexpr std::string_view window_title = "windowTitle";
inline constexpr std::string_view visual_runtime = "visualRuntime";
inline constexpr std::string_view modules = "modules";
inline constexpr std::string_view kind = "kind";
inline constexpr std::string_view cooldowns = "cooldowns";
inline constexpr std::string_view duration_ms = "durationMs";
inline constexpr std::string_view max_charges = "maxCharges";
inline constexpr std::string_view stack_charges = "stackCharges";
inline constexpr std::string_view retrigger = "retrigger";
inline constexpr std::string_view recovery = "recovery";
inline constexpr std::string_view cooldown_groups = "cooldownGroups";
inline constexpr std::string_view mode = "mode";
inline constexpr std::string_view hid_bindings = "hidBindings";
inline constexpr std::string_view device = "device";
inline constexpr std::string_view vid = "vid";
inline constexpr std::string_view pid = "pid";
inline constexpr std::string_view version = "version";
inline constexpr std::string_view usage_page = "usagePage";
inline constexpr std::string_view usage = "usage";
inline constexpr std::string_view descriptor_hash = "descriptorHash";
inline constexpr std::string_view serial = "serial";
inline constexpr std::string_view stable_slot = "stableSlot";
inline constexpr std::string_view control = "control";
inline constexpr std::string_view report_id = "reportId";
inline constexpr std::string_view link_collection = "linkCollection";
inline constexpr std::string_view activation = "activation";
inline constexpr std::string_view value = "value";
inline constexpr std::string_view threshold = "threshold";
inline constexpr std::string_view hysteresis = "hysteresis";
inline constexpr std::string_view allow_system_class = "allowSystemClass";
inline constexpr std::string_view panels = "panels";
inline constexpr std::string_view layout = "layout";
inline constexpr std::string_view style = "style";
inline constexpr std::string_view bar = "bar";
inline constexpr std::string_view width = "width";
inline constexpr std::string_view height = "height";
inline constexpr std::string_view fill_color = "fillColor";
inline constexpr std::string_view background_color = "backgroundColor";
inline constexpr std::string_view border_color = "borderColor";
inline constexpr std::string_view text_color = "textColor";
inline constexpr std::string_view show_text = "showText";
inline constexpr std::string_view hide_when_ready = "hideWhenReady";
inline constexpr std::string_view anchor_x = "anchorX";
inline constexpr std::string_view anchor_y = "anchorY";
inline constexpr std::string_view bindings = "bindings";
inline constexpr std::string_view cooldown_id = "cooldownId";
inline constexpr std::string_view chord = "chord";
inline constexpr std::string_view binding_id = "bindingId";
inline constexpr std::string_view code = "code";
inline constexpr std::string_view controller_index = "controllerIndex";

} // namespace layerc::core::profile_keys
