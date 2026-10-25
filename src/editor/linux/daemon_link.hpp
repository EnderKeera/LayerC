#pragma once

#include "document_commit.hpp"

#include "layerc/runtime/display_diagnostics.hpp"

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
  // V2.7 display session and provider rows; absent from V2.6 daemons.
  std::optional<layerc::runtime::DisplayStatusSnapshot> display;
  // The raw RuntimeDiagnostics payload, for the Runtime Diagnostics view.
  std::string payload_json;

  // One status-bar line: "LayerD 2.6.0 — X11 connected, 1 session", or
  // "LayerD 2.7.0 — Wayland (wlroots), XWayland connected, 1 session".
  [[nodiscard]] std::string summary() const;
  [[nodiscard]] bool wayland_session() const noexcept;
};

// Whether the editor runs inside a Wayland session (it always draws through
// X11, so there it is an XWayland client).
[[nodiscard]] bool wayland_environment();

// Detected Tasks and placement scan X11 windows: in a Wayland session they
// list XWayland clients only. The note says so and names LayerD's window
// provider; empty on X11.
[[nodiscard]] std::string wayland_scope_note(const DaemonStatus &status,
                                             bool wayland_environment);

[[nodiscard]] DaemonStatus query_daemon_status();

enum class ReloadState { acknowledged, daemon_unavailable, failed };

struct ReloadResult {
  ReloadState state{ReloadState::daemon_unavailable};
  std::string error;
};

// Asks the running daemon to reload profiles after a committed save.
[[nodiscard]] ReloadResult request_daemon_reload();

// Manual Save and background autosave phrase the same durable outcome
// differently; the wording is derived from the mode, never rewritten.
enum class SaveMode { manual, autosave };

// A disk save and a runtime reload are distinct outcomes: the text reports
// both, and a failed commit never claims a reload.
[[nodiscard]] std::string
save_report_text(const CommitOutcome &commit,
                 const std::optional<ReloadResult> &reload,
                 SaveMode mode = SaveMode::manual);

} // namespace layerc::editor
