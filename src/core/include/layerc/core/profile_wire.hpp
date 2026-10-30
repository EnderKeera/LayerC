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
inline constexpr std::array overlay_item_kind{
    EnumWireEntry{OverlayItemKind::cooldown, "cooldown"},
    EnumWireEntry{OverlayItemKind::timer, "timer"},
    EnumWireEntry{OverlayItemKind::counter, "counter"},
    EnumWireEntry{OverlayItemKind::indicator, "indicator"},
};
inline constexpr std::array overlay_timer_source{
    EnumWireEntry{OverlayTimerSource::automation, "automation"},
    EnumWireEntry{OverlayTimerSource::cooldown_active, "cooldownActive"},
};
inline constexpr std::array counter_display{
    EnumWireEntry{CounterDisplay::number, "number"},
    EnumWireEntry{CounterDisplay::pips, "pips"},
};

} // namespace layerc::core::profile_wire
