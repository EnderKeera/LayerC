#pragma once

#include "platform/win32/daemon_hub.hpp"
#include "platform/win32/daemon_hub_edit.hpp"
#include "platform/win32/trace_relay.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace layerc::editor {
namespace daemon_hub_detail {

using Clock = std::chrono::steady_clock;

struct Request final {
  // Trace requests have no waiting caller: their outcome is the relay's
  // subscription state.
  enum class Kind {
    status, reload, trace_subscribe, trace_unsubscribe, retry_visual, retry_automation,
  };

  Request(Kind request_kind, std::optional<CommittedProfile> profile,
          const Clock::time_point request_deadline)
      : kind(request_kind), committed(std::move(profile)),
        deadline(request_deadline) {}

  Kind kind;
  std::optional<CommittedProfile> committed;
  Clock::time_point deadline;
  std::condition_variable ready;
  bool completed{};
  DaemonStatus status_result;
  ReloadResult reload_result;
  layerc::core::LiveMessage retry_message;
  DaemonCommandResult retry_result;
  std::uint64_t trace_id{};
  std::atomic<bool> request_sent{};
};

void open_connection(const DaemonHubEnvironment &environment,
                     std::unique_ptr<LiveEditSession> &session,
                     PipeLiveEditTransport *&transport,
                     std::optional<DirectDragRelease> &carried);
void carry_session_release(DaemonHub::State &state, LiveEditSession &session,
                           std::optional<DirectDragRelease> &carried);
void reconnect_for_release(const DaemonHubEnvironment &environment,
                           std::unique_ptr<LiveEditSession> &session,
                           PipeLiveEditTransport *&transport,
                           std::optional<DirectDragRelease> &carried);
void publish_drag_release(DaemonHub::State &state, std::uint64_t swapped_seq,
                          const LiveEditSession *session, bool carried);

std::optional<std::string> start_worker_locked(
    const std::shared_ptr<DaemonHub::State> &state,
    const DaemonHubEnvironment &environment);

} // namespace daemon_hub_detail

struct DaemonHub::State {
  State();
  ~State();

  std::mutex mutex;
  std::mutex lifecycle_mutex;
  std::deque<std::shared_ptr<daemon_hub_detail::Request>> queue;
  std::shared_ptr<daemon_hub_detail::Request> active;
  std::thread worker;
  TraceRelay trace;
  // The on-window edit session: UI commands in order, and the events the
  // worker's HubEditDriver produced for the current id.
  std::deque<HubEditCommand> edit_commands;
  std::uint64_t edit_current{};
  std::uint64_t next_edit_id{};
  std::uint64_t drag_enable_seq{};
  std::uint64_t drag_released_seq{};
  std::optional<std::pair<std::uint64_t, bool>> unqueued_end;
  std::function<void()> before_release_publish_for_test;
  std::function<void(DirectDragRelease &)> on_release_carried_for_test;
  bool fail_edit_wake_for_test{};
  std::vector<OnWindowEditEvent> edit_events;
  std::function<void()> edit_listener;
  HANDLE wake_event{};
  bool stopping{};
  bool worker_failed{};
  std::string initialization_error;
  std::string worker_error;
};

} // namespace layerc::editor
