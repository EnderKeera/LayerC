#pragma once

#include "automation_trace_client.hpp"
#include "daemon_link.hpp"
#include "platform/on_window_edit.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

  [[nodiscard]] DaemonCommandResult retry_visual();
  [[nodiscard]] DaemonCommandResult retry_automation(
      const std::filesystem::path &profile, std::string_view automation_id);

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

  // The one on-window edit session over the same pipe. begin_edit replaces
  // an older session and returns the new nonzero id; the others act only
  // while that id is current. None waits: the worker connects, launching
  // LayerD when its pipe is absent, and queues events for take_edit_events.
  // The listener runs on the worker whenever an event is queued; it must
  // only post to the UI loop and never call back into the hub.
  [[nodiscard]] std::uint64_t begin_edit(std::uint64_t window,
                                         OnWindowEditBegin payload);
  void edit_live_profile(std::uint64_t session, std::string profile_json);
  void edit_anchor(std::uint64_t session, std::string panel_id, double x, double y,
                   bool final);
  void edit_direct_drag(std::uint64_t session, bool enabled);
  void end_edit(std::uint64_t session, bool notify) noexcept;
  [[nodiscard]] std::vector<OnWindowEditEvent> take_edit_events(std::uint64_t session);
  void set_edit_listener(std::function<void()> listener);

  void shutdown() noexcept;

  struct State;

private:
  DaemonHubEnvironment environment_;
  std::shared_ptr<State> state_;
};

} // namespace layerc::editor
