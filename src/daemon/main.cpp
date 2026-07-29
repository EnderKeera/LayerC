#include "daemon_app.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LayerC.LayerD.SingleInstance");
    if (mutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    layerc::daemon::DaemonApp app;
    const int result = app.run(instance, show_command);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}

