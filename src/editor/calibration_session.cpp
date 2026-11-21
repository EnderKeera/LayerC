#include "calibration_session.hpp"

#include "editor_prompt.hpp"
#include "editor_text.hpp"

namespace layerc::editor {

void show_calibration_failure(const HWND owner, const CalibrationFailure &failure) {
  if (const auto text = calibration_failure_text(failure)) {
    const auto title = to_wide(text->title);
    const auto message = to_wide(text->message);
    themed_message_box(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
  }
}

} // namespace layerc::editor
