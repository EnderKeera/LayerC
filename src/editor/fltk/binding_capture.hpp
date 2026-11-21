#pragma once
#include "layerc/core/types.hpp"
#include <optional>

namespace layerc::editor {
// Only input aimed at the modal capture dialog is read: physical key events,
// presses on its mouse capture area, and gamepad presses while focused.
// Existing generic HID bindings are not changed by this dialog.
[[nodiscard]] std::optional<layerc::core::InputChord> capture_focused_binding();
}
