#pragma once

#include <windows.h>

namespace layerc::editor {

class EditorApp {
public:
  int run(HINSTANCE instance, int show_command);
};

} // namespace layerc::editor