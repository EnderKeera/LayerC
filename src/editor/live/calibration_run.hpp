#pragma once

#include "layerc/core/calibration.hpp"

#include <functional>
#include <optional>
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

struct CalibrationWait {
  bool cancel{};
  std::optional<int> quit_code;
};

// Waits at most `milliseconds` for `process` while keeping the owner's UI
// loop responsive. quit_code: the outer loop received WM_QUIT; cancel: the
// owner closed. Either stops the session.
using CalibrationPump = std::function<CalibrationWait(HANDLE process, DWORD milliseconds)>;

[[nodiscard]] CalibrationPump win32_message_pump();
[[nodiscard]] std::variant<core::CalibrationResponse, CalibrationFailure>
run_calibration_session(HWND owner, const core::CalibrationRequest &request,
                        const CalibrationPump &pump = win32_message_pump());

struct CalibrationFailureText {
  std::string title, message; // UTF-8
};

// nullopt for cancelled: shutdown must not open another dialog.
[[nodiscard]] std::optional<CalibrationFailureText>
calibration_failure_text(const CalibrationFailure &failure);

} // namespace layerc::editor
