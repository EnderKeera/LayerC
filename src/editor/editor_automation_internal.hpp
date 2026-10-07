#pragma once

#include "automation_presets.hpp"
#include "layerc/core/automation.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace layerc::editor {

inline constexpr UINT_PTR automation_simulator_timer = 94'001;

using layerc::core::expr::literal;

enum class AutomationPaletteTarget { rule, action, variable };

struct AutomationPaletteEntry {
  const wchar_t *label;
  AutomationPaletteNode node;
  AutomationPaletteTarget target;
  bool requires_cooldown;
  bool requires_panel;
};

// IDC_AUTOMATION_PALETTE items, in combo order.
inline constexpr std::array automation_palette{
    AutomationPaletteEntry{L"Event: input pressed",
                           AutomationPaletteNode::input_rule,
                           AutomationPaletteTarget::rule, true, false},
    AutomationPaletteEntry{L"Event: timer fired",
                           AutomationPaletteNode::timer_rule,
                           AutomationPaletteTarget::rule, true, false},
    AutomationPaletteEntry{L"Action: trigger cooldown",
                           AutomationPaletteNode::trigger_action,
                           AutomationPaletteTarget::action, true, false},
    AutomationPaletteEntry{L"Action: create timer",
                           AutomationPaletteNode::timer_action,
                           AutomationPaletteTarget::action, true, false},
    AutomationPaletteEntry{L"Action: overlay visibility",
                           AutomationPaletteNode::overlay_action,
                           AutomationPaletteTarget::action, true, true},
    AutomationPaletteEntry{L"Variable: Boolean",
                           AutomationPaletteNode::flag_variable,
                           AutomationPaletteTarget::variable, false, false},
};

[[nodiscard]] inline const AutomationPaletteEntry *
automation_palette_entry(const LRESULT index) noexcept {
  return index < 0 || static_cast<std::size_t>(index) >=
                          automation_palette.size()
             ? nullptr
             : &automation_palette[static_cast<std::size_t>(index)];
}

[[nodiscard]] inline constexpr LPARAM automation_tree_item_data(
    const int rule, const int action) noexcept {
  return static_cast<LPARAM>((rule + 1) | ((action + 1) << 16));
}

[[nodiscard]] inline constexpr int
automation_tree_rule(const LRESULT data) noexcept {
  return static_cast<int>(static_cast<unsigned long long>(data) & 0xffffU) - 1;
}

[[nodiscard]] inline constexpr int
automation_tree_action(const LRESULT data) noexcept {
  return static_cast<int>((static_cast<unsigned long long>(data) >> 16) &
                          0xffffU) -
         1;
}

[[nodiscard]] std::wstring event_label(layerc::core::AutomationEventKind kind);
[[nodiscard]] std::wstring
action_label(layerc::core::AutomationActionKind kind);

} // namespace layerc::editor
