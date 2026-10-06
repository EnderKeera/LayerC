#include "layerc/platform/platform_events.hpp"
#include "layerc/platform/win32_handle.hpp"

#include <windows.h>

namespace layerc::platform {

bool signal_named_event(const wchar_t *const name) {
  const OwnedHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, name));
  return event.valid() && SetEvent(event.get()) != FALSE;
}

bool signal_settings_reload() {
  return signal_named_event(settings_reload_event_name);
}

} // namespace layerc::platform
