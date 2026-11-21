#pragma once

#include "automation_trace_client.hpp"
#include "daemon_link.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <windows.h>

namespace layerc::editor {

// What the hub needs from the machine. Tests name a private pipe, pin their
// own process as the server, and count launches and reload signals.
struct DaemonHubEnvironment {
  std::wstring pipe_name;
  // Nonzero pins the server process; zero trusts a sibling layerd.exe.
  DWORD expected_server_process_id{};
  std::function<bool()> start_daemon;
  std::function<bool()> signal_reload;
  // Bounds one request, from connecting through LayerD's reply.
  std::chrono::milliseconds reply_timeout{std::chrono::seconds(2)};
};

// LayerD's live-edit pipe, the sibling launch, and its named reload event.
[[nodiscard]] DaemonHubEnvironment product_daemon_hub_environment();

class DaemonHub;
// The editor process's one hub over product_daemon_hub_environment().
[[nodiscard]] DaemonHub &product_daemon_hub();

// The Windows editor's link to LayerD. One owned worker holds the
// authenticated live-edit pipe and answers status and reload requests; it
// starts and connects only when a request needs it, then blocks on its wake
// event and the pipe's I/O events. Callers block for at most reply_timeout.
// shutdown() fails pending requests and joins the worker. Toolkit-free.
class DaemonHub final {
public:
  explicit DaemonHub(DaemonHubEnvironment environment);
  ~DaemonHub();
  DaemonHub(const DaemonHub &) = delete;
  DaemonHub &operator=(const DaemonHub &) = delete;

  [[nodiscard]] DaemonStatus status();
  // A committed profile goes to LayerD under the editor's revision and is
  // confirmed by its reply. Without one (delete, import) LayerD is signalled
  // to reload from disk, which has no reply.
  [[nodiscard]] ReloadResult
  reload(const std::optional<CommittedProfile> &committed);

  // The automation trace subscription LayerD pushes over the same pipe
  // (TraceRelay). subscribe_trace starts a fresh subscription and returns
  // its id; the others act only while that id is current. None of them
  // waits for LayerD: the worker subscribes, and polls retry a lapsed or
  // failed subscription until it ends.
  [[nodiscard]] std::uint64_t subscribe_trace();
  void poll_trace(std::uint64_t subscription);
  [[nodiscard]] std::optional<AutomationTraceUpdate>
  take_trace(std::uint64_t subscription);
  void unsubscribe_trace(std::uint64_t subscription) noexcept;

  void shutdown() noexcept;

  struct State;

private:
  DaemonHubEnvironment environment_;
  std::shared_ptr<State> state_;
};

} // namespace layerc::editor
