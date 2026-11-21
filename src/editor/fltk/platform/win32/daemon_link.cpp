#include "daemon_link.hpp"
#include "status_kind.hpp"

#include "platform/win32/daemon_hub.hpp"

#include "layerc/runtime/runtime_services.hpp"

namespace layerc::editor {
namespace {

DaemonHub &daemon_hub() {
  static DaemonHub hub(product_daemon_hub_environment());
  return hub;
}

} // namespace

std::string DaemonStatus::summary() const {
  if (!running) {
    const std::string text = socket_missing ? "LayerD is not running"
                                            : "LayerD status unavailable";
    return text + (error.empty() ? "" : " (" + error + ")");
  }

  std::string text = "LayerD " + (version.empty() ? std::string("running") : version);
  text += " — " + std::to_string(sessions) +
          (sessions == 1 ? " session" : " sessions");
  if (!error.empty()) text += " (" + error + ")";
  return text;
}

StatusKind DaemonStatus::kind() const noexcept {
  if (!running) return StatusKind::failure;
  if (!error.empty()) return StatusKind::warning;
  for (const auto &capability : capabilities) {
    if (capability.availability == layerc::runtime::CapabilityAvailability::degraded ||
        capability.availability == layerc::runtime::CapabilityAvailability::unavailable)
      return StatusKind::warning;
  }
  return StatusKind::success;
}

bool DaemonStatus::wayland_session() const noexcept { return false; }

bool wayland_environment() { return false; }

std::string wayland_scope_note(const DaemonStatus &, const bool) { return {}; }

DaemonStatus query_daemon_status() { return daemon_hub().status(); }

ReloadResult request_daemon_reload(
    const std::optional<CommittedProfile> &committed) {
  return daemon_hub().reload(committed);
}

void shutdown_daemon_link() noexcept {
  try {
    daemon_hub().shutdown();
  } catch (...) {
  }
}

} // namespace layerc::editor
