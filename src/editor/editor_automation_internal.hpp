#pragma once

#include "layerc/core/automation.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace layerc::editor {

inline constexpr UINT_PTR automation_simulator_timer = 94'001;

struct AutomationNodeRect {
  int rule{};
  int action{-1};
  RECT rect{};
};

[[nodiscard]] std::wstring event_label(layerc::core::AutomationEventKind kind);
[[nodiscard]] std::wstring
action_label(layerc::core::AutomationActionKind kind);
[[nodiscard]] std::vector<AutomationNodeRect>
automation_node_rects(const RECT &canvas,
                      const layerc::core::AutomationDefinition &automation,
                      int zoom);

} // namespace layerc::editor
