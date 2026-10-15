#include "daemon_link.hpp"

#include "control_channel.hpp"
#include "runtime_diagnostics_view.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/runtime/diagnostics.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <sys/stat.h>
#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <variant>

namespace layerc::editor {
namespace {

namespace control = layerc::platform;

// Only a successful inspection proving absence permits an offline-save
// classification. Permission/path errors must retain the transport failure.
bool daemon_socket_absent() {
  std::string warning;
  const auto path = control::control_socket_path(warning);
  if (!path) return false;
  struct stat status{};
  // lstat retains ENOTDIR and treats dangling/self-referential symlinks as
  // present malformed paths. exists() can normalize such faults to absence.
  if (::lstat(path->c_str(), &status) == 0) return false;
  return errno == ENOENT;
}

} // namespace

std::string DaemonStatus::summary() const {
  if (!running) {
    const std::string text = socket_missing
        ? "LayerD is not running" : "LayerD status unavailable";
    return text + (error.empty() ? "" : " (" + error + ")");
  }
  std::string text = "LayerD " + (version.empty() ? std::string("running") : version);
  if (wayland_session()) {
    // X11 here is the XWayland connection LayerD may use for degraded paths.
    text += " \u2014 Wayland (" +
            std::string(layerc::core::enum_wire_name(layerc::runtime::display_compositor_wire,
                                                     display->compositor)
                            .value_or("unknown")) +
            "), XWayland ";
    text += x11_connected ? "connected" : "unavailable";
  } else {
    text += x11_connected ? " \u2014 X11 connected" : " \u2014 X11 unavailable";
  }
  if (!x11_connected && !x11_reason.empty()) text += " (" + x11_reason + ")";
  text += ", " + std::to_string(sessions) + (sessions == 1 ? " session" : " sessions");
  if (!error.empty()) text += " (" + error + ")";
  return text;
}

bool DaemonStatus::wayland_session() const noexcept {
  return display && display->session == layerc::runtime::DisplaySession::wayland;
}

bool wayland_environment() {
  const char *socket = std::getenv("WAYLAND_DISPLAY");
  const char *type = std::getenv("XDG_SESSION_TYPE");
  return (socket != nullptr && *socket != '\0') ||
         (type != nullptr && std::string_view{type} == "wayland");
}

std::string wayland_scope_note(const DaemonStatus &status, const bool wayland_environment) {
  if (!status.wayland_session() && !wayland_environment) return {};
  std::string note = "Wayland session: only XWayland windows are listed (degraded); native "
                     "Wayland windows are not shown here.";
  if (status.wayland_session())
    note += " LayerD tracks windows with " +
            display_provider_text(status.display->providers.windows) + ".";
  else
    note += status.running ? " LayerD did not report a window provider."
                           : " LayerD is not running, so its window provider is unknown.";
  return note;
}

DaemonStatus query_daemon_status() {
  namespace json = layerc::core::json;
  namespace runtime = layerc::runtime;
  DaemonStatus status;
  std::string error;
  const auto response = control::control_roundtrip(
      control::encode_control_request(control::ControlCommand::status), error);
  if (!response) {
    status.socket_missing = daemon_socket_absent();
    status.error = std::move(error);
    return status;
  }
  status.running = true;
  const auto envelope = json::parse_bounded(*response, control::control_frame_limit);
  const auto *root = std::get_if<json::Value>(&envelope);
  json::Reader reader;
  const auto *payload = root ? reader.object(*root, "payload", "$") : nullptr;
  if (payload == nullptr) {
    status.error = "status payload unavailable";
    return status;
  }
  auto &payload_json = status.payload_json;
  json::append_value(payload_json, *payload);
  const auto parsed = runtime::parse_runtime_diagnostics_json(payload_json);
  const auto *diagnostics = std::get_if<runtime::RuntimeDiagnostics>(&parsed);
  if (diagnostics == nullptr) {
    status.error = "status payload invalid";
    return status;
  }
  status.version = diagnostics->package_version;
  status.sessions = diagnostics->sessions.size();
  if (diagnostics->linux_extension) {
    const auto &x11 = diagnostics->linux_extension->x11;
    status.x11_connected = x11.connected;
    status.x11_reason = x11.reason;
    for (const auto &capability : diagnostics->linux_extension->capabilities) {
      CapabilityLine line{
          .name = std::string(runtime::capability_name(capability.capability)),
          .availability =
              std::string(runtime::availability_name(capability.availability)),
          .reasons = {}};
      for (const auto *list : {&capability.reasons, &capability.diagnostics})
        for (const auto &reason : *list)
          line.reasons += (line.reasons.empty() ? "" : ", ") + reason;
      status.capabilities.push_back(std::move(line));
    }
    status.display = diagnostics->linux_extension->display;
  }
  return status;
}

ReloadResult request_daemon_reload() {
  std::string error;
  const auto response = control::control_roundtrip(
      control::encode_control_request(control::ControlCommand::reload), error);
  if (response) return {ReloadState::acknowledged, {}};
  if (daemon_socket_absent())
    return {ReloadState::daemon_unavailable, std::move(error)};
  return {ReloadState::failed, std::move(error)};
}

std::string save_report_text(const CommitOutcome &commit,
                             const std::optional<ReloadResult> &reload) {
  switch (commit.result) {
  case CommitResult::ok:
    break;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    return "Not saved: finish the invalid field first.";
  case CommitResult::upgrade_declined:
    return "Not saved: the profile format upgrade was declined.";
  case CommitResult::blocked_by_sidecar:
    return "Not saved: repair the invalid visual or automation settings first.";
  case CommitResult::write_failed:
    // The final durability barrier can fail after every file was published.
    if (commit.write_error == layerc::platform::ProfileWriteError::durability_unconfirmed)
      return "Save unconfirmed: the files may already be on disk, but their durability "
             "could not be confirmed. These edits are kept; Save again.";
    return "Not saved. " + write_failure_text(commit.write_error) + ".";
  }
  if (!reload) return "Saved.";
  switch (reload->state) {
  case ReloadState::acknowledged:
    return "Saved \u2014 LayerD acknowledged the reload request.";
  case ReloadState::daemon_unavailable:
    return "Saved. LayerD is not running; the change applies when it starts.";
  case ReloadState::failed:
    break;
  }
  return "Saved; LayerD's reload could not be confirmed" +
         (reload->error.empty() ? std::string(".") : ": " + reload->error + ".");
}

} // namespace layerc::editor
