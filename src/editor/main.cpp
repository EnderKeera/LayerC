#include "editor_app.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  layerc::editor::EditorApp app;
  return app.run(instance, show_command);
}