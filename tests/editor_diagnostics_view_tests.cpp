// The shared Runtime Diagnostics text both editors show.
#include "runtime_diagnostics_view.hpp"
#include "test_support.hpp"

#include "layerc/runtime/diagnostics.hpp"

#include <algorithm>
#include <string>

namespace {
using layerc::test::expect;
namespace runtime = layerc::runtime;

bool has_line(const layerc::editor::RuntimeDiagnosticsView &view, const std::string &text) {
  return std::ranges::any_of(view.lines, [&](const std::string &line) {
    return line.find(text) != std::string::npos;
  });
}

void summary_states() {
  using layerc::editor::runtime_diagnostics_view;
  const auto offline = runtime_diagnostics_view("", false, false, true);
  expect(offline.lines.size() == 4 && offline.lines[0] == "Connection: LayerD disconnected" &&
             offline.lines[3] == "Last error: LayerD is unavailable",
         "no snapshot keeps the four-line summary and names the unavailable daemon");
  const auto pending = runtime_diagnostics_view("", true, false, false);
  expect(pending.lines[0] == "Connection: LayerD connected; authentication pending" &&
             pending.lines[1] == "Active profile: Unavailable",
         "an unauthenticated connection reports pending authentication");
  const auto waiting = runtime_diagnostics_view("", true, true, false);
  expect(waiting.lines[1] == "Active profile: Waiting for snapshot" &&
             waiting.lines[3] == "Last error: No diagnostics snapshot received",
         "an authenticated connection without a snapshot waits for one");
  const auto invalid = runtime_diagnostics_view("{}", true, true, true);
  expect(invalid.lines.size() == 4 && invalid.lines[3].starts_with("Last error: Invalid diagnostics snapshot ("),
         "an invalid snapshot is reported, never detailed");
}

void windows_summary_and_linux_details() {
  runtime::RuntimeDiagnostics diagnostics;
  diagnostics.process.pid = 42;
  diagnostics.process.working_set_bytes = 12U * 1024U * 1024U;
  diagnostics.sessions.push_back({.profile_id = "background", .window = 0x10});
  diagnostics.sessions.push_back({.profile_id = "fixture", .window = 0x2a, .foreground = true,
                                  .overlay_windows = 1, .last_input_match = "dash"});
  diagnostics.services.push_back({.capability = runtime::Capability::overlay,
                                  .state = runtime::ServiceState::active, .consumers = 1,
                                  .last_error = "overlay lost"});
  diagnostics.automation_faults.push_back("checkpoint failed");
  runtime::LinuxStatusSnapshot linux_status;
  linux_status.x11.connected = true;
  linux_status.x11.xi2 = {.available = true, .major = 2, .minor = 4};
  linux_status.capabilities.push_back({.capability = runtime::Capability::visual,
                                .availability = runtime::CapabilityAvailability::degraded,
                                .reasons = {"profile_invalid"}, .diagnostics = {"bad region"}});
  linux_status.sessions = diagnostics.sessions;
  const auto json = runtime::serialize_linux_status_json(diagnostics, linux_status);

  const auto summary = layerc::editor::runtime_diagnostics_view(json, true, true, false);
  expect(summary.lines.size() == 4 && summary.lines[1] == "Active profile: fixture" &&
             summary.lines[2] == "Isolated visual worker: Not active" &&
             summary.lines[3] == "Last error: overlay lost" && !summary.retry_visual,
         "the summary picks the foreground session and the first reported error");
  const auto detailed = layerc::editor::runtime_diagnostics_view(json, true, true, true);
  expect(detailed.lines.size() > 4 && has_line(detailed, "process 42") &&
             has_line(detailed, "resident 12.0 MiB") && has_line(detailed, "X11: connected") &&
             has_line(detailed, "XI2 2.4") &&
             has_line(detailed, "visual: degraded (profile_invalid; bad region)") &&
             has_line(detailed, "overlay: active, 1 consumer, 0 faults; last error: overlay lost") &&
             has_line(detailed, "fixture on window 0x2a, foreground, 1 overlay window; last input: dash") &&
             has_line(detailed, "checkpoint failed"),
         "details list process, X11, capability, service, session and automation state");
}
void worker_states_and_error_precedence() {
  using layerc::editor::runtime_diagnostics_view;
  const auto view = [](const runtime::RuntimeDiagnostics &value) {
    return runtime_diagnostics_view(runtime::serialize_runtime_diagnostics_json(value), true, true, false);
  };
  runtime::RuntimeDiagnostics diagnostics;
  diagnostics.settings.last_error = "settings";
  expect(view(diagnostics).lines[3] == "Last error: settings", "settings errors are the last fallback");
  diagnostics.gamebar.last_error = "gamebar";
  expect(view(diagnostics).lines[3] == "Last error: gamebar", "Game Bar errors precede settings");
  diagnostics.services.push_back({.capability = runtime::Capability::visual, .last_error = "service"});
  expect(view(diagnostics).lines[3] == "Last error: service", "service errors precede Game Bar");
  diagnostics.helpers = {{.window = 1, .running = true}, {.window = 2, .running = true}};
  expect(view(diagnostics).lines[2] == "Isolated visual worker: Running (2)", "all helpers running");
  diagnostics.helpers[1].running = false;
  expect(view(diagnostics).lines[2] == "Isolated visual worker: Degraded (1 of 2 running)", "partially running");
  diagnostics.helpers[0].running = false;
  expect(view(diagnostics).lines[2] == "Isolated visual worker: Stopped (2)", "no helper running");
  diagnostics.helpers[1].quarantined = true;
  diagnostics.helpers[1].last_error = "helper";
  const auto quarantined = view(diagnostics);
  expect(quarantined.lines[2] == "Isolated visual worker: Quarantined (1 of 2)" &&
             quarantined.lines[3] == "Last error: helper" && quarantined.retry_visual,
         "a quarantined helper offers retry and its error takes precedence");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(summary_states),
                 LAYERC_TEST_CASE(windows_summary_and_linux_details),
                 LAYERC_TEST_CASE(worker_states_and_error_precedence))
