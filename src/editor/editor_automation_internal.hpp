#pragma once

#include "layerc/core/automation.hpp"

#include <windows.h>

#include <string>

namespace layerc::editor {

inline constexpr UINT_PTR automation_simulator_timer = 94'001;

enum class AutomationPreset {
  blank,
  input_cooldown,
  repeating_session_timer,
  ready_overlay_pulse,
};

[[nodiscard]] layerc::core::AutomationDefinition make_automation_preset(
    AutomationPreset preset, std::string automation_id,
    std::string cooldown_id = {}, std::string panel_id = {});

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
