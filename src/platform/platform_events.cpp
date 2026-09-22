#include "layerc/platform/platform_events.hpp"

#include <windows.h>

namespace layerc::platform {

bool signal_settings_reload() {
  const HANDLE event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, settings_reload_event_name);
  if (event == nullptr)
    return false;
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

} // namespace layerc::platform
