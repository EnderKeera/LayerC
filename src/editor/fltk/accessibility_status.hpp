#pragma once

#include <string>

namespace layerc::editor {

enum class AccessibilityStatusState { active, waiting, unavailable };

struct AccessibilityStatus {
  AccessibilityStatusState state{AccessibilityStatusState::waiting};
  std::string reason;
};

[[nodiscard]] std::string accessibility_status_line(const AccessibilityStatus &status);

} // namespace layerc::editor
