#pragma once

#include <optional>
#include <utility>

class Fl_Window;

namespace layerc::editor::a11y {

// Frontend-private toolkit glue, not the toolkit-free platform-services API.
// The client origin is in physical screen pixels; absent for an unmapped window.
[[nodiscard]] std::optional<std::pair<int, int>>
window_screen_origin(Fl_Window &window);

} // namespace layerc::editor::a11y
