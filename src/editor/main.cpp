#include "editor_app.hpp"

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  (void)SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  layerc::editor::EditorApp app;
  return app.run(instance, show_command);
}
