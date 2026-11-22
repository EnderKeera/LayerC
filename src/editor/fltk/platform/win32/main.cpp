#include "startup.hpp"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ PWSTR command_line, _In_ int) {
  return layerc::editor::run_windows_editor(
      instance, command_line != nullptr ? command_line : L"");
}
