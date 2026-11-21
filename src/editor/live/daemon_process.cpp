#include "daemon_process.hpp"

#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/platform/window_query.hpp"

#include <string>

#include <windows.h>

namespace layerc::editor {

bool start_daemon() {
  const auto daemon = layerc::platform::sibling_image_path(L"layerd.exe");
  if (daemon.empty()) return false;
  std::wstring command_line =
      layerc::platform::quote_command_argument(daemon.wstring());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created =
      CreateProcessW(daemon.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, daemon.parent_path().c_str(),
                     &startup, &process);
  const layerc::platform::OwnedHandle thread(process.hThread);
  const layerc::platform::OwnedHandle launched(process.hProcess);
  return created != FALSE;
}

} // namespace layerc::editor
