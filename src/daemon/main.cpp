#include "daemon_app.hpp"

#include <memory>

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  (void)SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HANDLE mutex =
      CreateMutexW(nullptr, TRUE, L"Local\\LayerC.LayerD.SingleInstance");
  if (mutex == nullptr)
    return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  const auto app = std::make_unique<layerc::daemon::DaemonApp>();
  const int result = app->run(instance, show_command);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return result;
}
