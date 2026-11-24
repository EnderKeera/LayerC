#include "platform/win32/daemon_hub_internal.hpp"

#include <algorithm>

namespace layerc::editor {
namespace {
std::atomic<DaemonHub *> product_hub{};
}
namespace daemon_hub_detail {

void open_connection(const DaemonHubEnvironment &environment,
                     std::unique_ptr<LiveEditSession> &session,
                     PipeLiveEditTransport *&transport,
                     std::optional<DirectDragRelease> &carried) {
  auto owned_transport = std::make_unique<PipeLiveEditTransport>(
      environment.pipe_name, environment.expected_server_process_id);
  transport = owned_transport.get();
  session = std::make_unique<LiveEditSession>(std::move(owned_transport));
  const auto seed = (GetTickCount64() << 16U) | GetCurrentProcessId();
  session->seed_revision(static_cast<layerc::core::ProfileRevision>(seed));
  if (carried) {
    session->adopt_release(std::move(*carried));
    carried.reset();
  }
}

void carry_session_release(DaemonHub::State &state, LiveEditSession &session,
                           std::optional<DirectDragRelease> &carried) {
  (void)session.end(false);
  if (auto release = session.take_release()) {
    std::function<void(DirectDragRelease &)> carried_for_test;
    {
      std::lock_guard lock(state.mutex);
      carried_for_test = state.on_release_carried_for_test;
    }
    if (carried_for_test) carried_for_test(*release);
    carried = std::move(release);
  }
}

void reconnect_for_release(const DaemonHubEnvironment &environment,
                           std::unique_ptr<LiveEditSession> &session,
                           PipeLiveEditTransport *&transport,
                           std::optional<DirectDragRelease> &carried) {
  if (!carried) return;
  if (carried->probe && carried->probe()) {
    carried.reset();
  } else if (!session && !environment.pipe_name.empty() &&
             WaitNamedPipeW(environment.pipe_name.c_str(), 1) != FALSE) {
    open_connection(environment, session, transport, carried);
  }
}

void publish_drag_release(DaemonHub::State &state, const std::uint64_t swapped_seq,
                          const LiveEditSession *session, const bool carried) {
  const bool session_settled = !session || session->direct_drag_settled();
  if (carried || !session_settled) return;
  std::unique_lock lock(state.mutex);
  if (auto before_publish = std::exchange(state.before_release_publish_for_test, {})) {
    lock.unlock();
    before_publish();
    lock.lock();
  }
  state.drag_released_seq = std::max(state.drag_released_seq, swapped_seq);
}

} // namespace daemon_hub_detail

DaemonHub *product_daemon_hub_if_created() noexcept { return product_hub.load(); }

DaemonHub::DaemonHub(DaemonHubEnvironment environment)
    : environment_(std::move(environment)), state_(std::make_shared<State>()) {
  if (environment_.product) product_hub.store(this);
}

DaemonHub::~DaemonHub() {
  if (environment_.product) product_hub.store(nullptr);
  shutdown();
}

bool DaemonHub::direct_drag_released() const {
  std::lock_guard lock(state_->mutex);
  return state_->drag_released_seq == state_->drag_enable_seq;
}

} // namespace layerc::editor
