#pragma once

class Fl_Window;

namespace layerc::editor {

void refresh_editor_window_chrome();
[[nodiscard]] bool verify_editor_window_capture(Fl_Window &window);

} // namespace layerc::editor
