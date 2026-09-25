#include "layerc/platform/platform_events.hpp"

#include <windows.h>

namespace layerc::platform {

bool signal_named_event(const wchar_t *const name) {
  const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
  if (event == nullptr)
    return false;
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

bool signal_settings_reload() {
  return signal_named_event(settings_reload_event_name);
}

} // namespace layerc::platform
