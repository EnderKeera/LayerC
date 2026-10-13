#pragma once

#include "document_commit.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// The Linux editor's authenticated LayerC.Control v1 client: daemon status
// for the connection/fault display and the reload request that follows a
// committed save. Toolkit-free; every call is a bounded blocking roundtrip.
namespace layerc::editor {

struct CapabilityLine {
  std::string name;
  std::string availability;
  // Stable reason tokens and current fault text, joined for display.
  std::string reasons;
};

struct DaemonStatus {
  bool running{};
  // A successful inspection proved the control socket absent, not merely
  // inaccessible. Otherwise a failed query is unavailable, not "not running".
  bool socket_missing{};
  // Why the status is unavailable, or a payload problem while running.
  std::string error;
  std::string version;
  bool x11_connected{};
  std::string x11_reason;
  std::size_t sessions{};
  std::vector<CapabilityLine> capabilities;
  // The raw RuntimeDiagnostics payload, for the Runtime Diagnostics view.
  std::string payload_json;

  // One status-bar line: "LayerD 2.6.0 — X11 connected, 1 session".
  [[nodiscard]] std::string summary() const;
};

[[nodiscard]] DaemonStatus query_daemon_status();

enum class ReloadState { acknowledged, daemon_unavailable, failed };

struct ReloadResult {
  ReloadState state{ReloadState::daemon_unavailable};
  std::string error;
};

// Asks the running daemon to reload profiles after a committed save.
[[nodiscard]] ReloadResult request_daemon_reload();

// A disk save and a runtime reload are distinct outcomes: the text reports
// both, and a failed commit never claims a reload.
[[nodiscard]] std::string
save_report_text(const CommitOutcome &commit,
                 const std::optional<ReloadResult> &reload);

} // namespace layerc::editor
