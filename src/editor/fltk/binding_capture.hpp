#pragma once
#include "layerc/core/types.hpp"
#include <optional>

namespace layerc::editor {
// Only input aimed at the focused modal capture dialog is read: key events it
// receives (XKB physical names), presses on its mouse capture area, and XI2
// gamepad-class button presses while it holds the X input focus. Uses
// FLTK's X11 connection; Generic HID capture is unavailable on Linux.
[[nodiscard]] std::optional<layerc::core::InputChord> capture_focused_binding();
}
