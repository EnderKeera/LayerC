#include "layerc/runtime/diagnostics.hpp"

namespace layerc::runtime {

std::string serialize_linux_status_json(const RuntimeDiagnostics &diagnostics,
                                        const LinuxStatusSnapshot &linux) {
  // Preserve the caller's shared session rows; the native snapshot only
  // supplies the Linux probes and availability extension.
  auto snapshot = diagnostics;
  snapshot.linux_extension = LinuxDiagnostics{linux.x11, linux.capabilities, linux.display};
  return serialize_runtime_diagnostics_json(snapshot);
}

} // namespace layerc::runtime
