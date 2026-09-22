#pragma once

#include "layerc/core/enum_wire.hpp"
#include "layerc/core/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace layerc::core::profile_wire {

inline constexpr std::array retrigger{
    EnumWireEntry{RetriggerPolicy::ignore, "ignore"},
    EnumWireEntry{RetriggerPolicy::restart, "restart"},
};
inline constexpr std::array recovery{
    EnumWireEntry{RecoveryMode::single, "single"},
    EnumWireEntry{RecoveryMode::sequential, "sequential"},
    EnumWireEntry{RecoveryMode::parallel, "parallel"},
};
inline constexpr std::array group_mode{
    EnumWireEntry{CooldownGroupMode::trigger_propagation,
                  "triggerPropagation"},
    EnumWireEntry{CooldownGroupMode::shared_pool, "sharedPool"},
};
inline constexpr std::array panel_layout{
    EnumWireEntry{PanelLayout::row, "row"},
    EnumWireEntry{PanelLayout::column, "column"},
    EnumWireEntry{PanelLayout::grid, "grid"},
};
inline constexpr std::array panel_style{
    EnumWireEntry{PanelStyle::card, "card"},
    EnumWireEntry{PanelStyle::bar, "bar"},
};
inline constexpr std::array hid_activation{
    EnumWireEntry{HidActivationMode::button, "button"},
    EnumWireEntry{HidActivationMode::equals, "equals"},
    EnumWireEntry{HidActivationMode::axis_positive, "axisPositive"},
    EnumWireEntry{HidActivationMode::axis_negative, "axisNegative"},
    EnumWireEntry{HidActivationMode::relative_positive, "relativePositive"},
    EnumWireEntry{HidActivationMode::relative_negative, "relativeNegative"},
};
inline constexpr std::array visual_runtime{
    EnumWireEntry{VisualRuntime::inline_process, "inline"},
    EnumWireEntry{VisualRuntime::isolated, "isolated"},
};

struct ModuleEntry {
  ProfileModuleKind value;
  std::string_view name;
  std::uint32_t maximum_version;
  std::uint32_t minimum_profile_schema;
};
inline constexpr std::array modules{
    ModuleEntry{ProfileModuleKind::visual, "visual", 2, 1},
    ModuleEntry{ProfileModuleKind::automation, "automation", 1, 4},
    ModuleEntry{ProfileModuleKind::companion, "companion", 1, 1},
};

template <typename Enum, std::size_t Size>
[[nodiscard]] constexpr std::optional<std::string_view>
name(const std::array<EnumWireEntry<Enum>, Size> &entries,
     const Enum value) noexcept {
  return enum_wire_name<Enum>(entries, value);
}

template <typename Enum, std::size_t Size>
[[nodiscard]] constexpr std::optional<Enum>
parse(const std::array<EnumWireEntry<Enum>, Size> &entries,
      const std::string_view value) noexcept {
  return parse_enum_wire<Enum>(entries, value);
}

[[nodiscard]] constexpr const ModuleEntry *
module(const ProfileModuleKind kind) noexcept {
  for (const auto &entry : modules)
    if (entry.value == kind) return &entry;
  return nullptr;
}
[[nodiscard]] constexpr const ModuleEntry *
module(const std::string_view name) noexcept {
  for (const auto &entry : modules)
    if (entry.name == name) return &entry;
  return nullptr;
}

} // namespace layerc::core::profile_wire
