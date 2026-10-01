#pragma once

#include "layerc/core/calibration.hpp"

#include <string>
#include <variant>

#include <windows.h>

namespace layerc::editor {

enum class CalibrationFailureKind {
  cancelled,
  missing_image,
  channel,
  launch,
  monitor,
  request_timeout,
  session_timeout,
  write,
  read,
  invalid_response,
  failed_response,
};

struct CalibrationFailure {
  CalibrationFailureKind kind;
  std::string detail;
};

[[nodiscard]] std::variant<layerc::core::CalibrationResponse, CalibrationFailure>
run_calibration_session(HWND owner, const layerc::core::CalibrationRequest &request);
void show_calibration_failure(HWND owner, const CalibrationFailure &failure);

} // namespace layerc::editor
