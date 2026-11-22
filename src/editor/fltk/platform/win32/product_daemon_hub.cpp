#include "platform/win32/daemon_hub.hpp"

namespace layerc::editor {

DaemonHub &product_daemon_hub() {
  static DaemonHub hub(product_daemon_hub_environment());
  return hub;
}

} // namespace layerc::editor
