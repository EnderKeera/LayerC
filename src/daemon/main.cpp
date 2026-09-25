#include "daemon_app.hpp"

#include "layerc/platform/win32_handle.hpp"

#include <memory>

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  (void)SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  using layerc::platform::SingleInstanceGuard;
  const SingleInstanceGuard guard(L"Local\\LayerC.LayerD.SingleInstance");
  if (guard.state() != SingleInstanceGuard::State::acquired)
    return guard.state() == SingleInstanceGuard::State::failed ? 1 : 0;
  return std::make_unique<layerc::daemon::DaemonApp>()->run(instance,
                                                          show_command);
}
