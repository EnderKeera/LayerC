#pragma once
#include "layerc/core/types.hpp"
#include <optional>

namespace layerc::editor {
// Only local events delivered to the focused modal editor window are read.
// Uses FLTK's X11 connection and the existing XKB physical-name mapping.
[[nodiscard]] std::optional<layerc::core::InputChord> capture_focused_binding();
}
