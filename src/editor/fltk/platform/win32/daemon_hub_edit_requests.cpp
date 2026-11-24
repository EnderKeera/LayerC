#include "platform/win32/daemon_hub_internal.hpp"

namespace layerc::editor {

namespace {
using daemon_hub_detail::start_worker_locked;

// Queues one edit command, starting the worker on first demand. The caller
// holds lifecycle_mutex and mutex. Returns the reason the link cannot take
// it, if any.
std::optional<std::string> enqueue_edit_locked(
    const std::shared_ptr<DaemonHub::State> &state,
    const DaemonHubEnvironment &environment, HubEditCommand command) {
  if (auto failure = start_worker_locked(state, environment)) return failure;
  state->edit_commands.push_back(std::move(command));
  const auto &queued = state->edit_commands.back();
  if (state->fail_edit_wake_for_test || SetEvent(state->wake_event) == FALSE) {
    if (queued.kind == HubEditCommand::Kind::end) return std::nullopt;
    state->edit_commands.pop_back();
    return "The LayerD link wake event failed.";
  }
  if (queued.kind == HubEditCommand::Kind::direct_drag && queued.flag)
    ++state->drag_enable_seq;
  return std::nullopt;
}

// Ends the current session with an unavailable reason. Returns the listener
// to run after the locks are released.
std::function<void()> fail_edit_locked(DaemonHub::State &state, std::string reason) {
  state.edit_current = 0;
  state.edit_events.push_back({.kind = OnWindowEditEvent::Kind::ended,
                               .reason = OnWindowEditEvent::EndReason::unavailable,
                               .text = std::move(reason)});
  return state.edit_listener;
}

// Queues a command while its session is current; a failure ends the session.
void send_edit(const std::shared_ptr<DaemonHub::State> &state,
               const DaemonHubEnvironment &environment, HubEditCommand command) {
  std::function<void()> listener;
  {
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    std::lock_guard lock(state->mutex);
    if (command.id == 0 || command.id != state->edit_current) return;
    auto failure = enqueue_edit_locked(state, environment, std::move(command));
    if (!failure) return;
    listener = fail_edit_locked(*state, std::move(*failure));
  }
  if (listener) listener();
}

} // namespace

std::uint64_t DaemonHub::begin_edit(const std::uint64_t window,
                                    OnWindowEditBegin payload) {
  std::function<void()> listener;
  std::uint64_t id{};
  {
    std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
    std::lock_guard lock(state_->mutex);
    id = ++state_->next_edit_id;
    state_->edit_current = id;
    state_->edit_events.clear();
    auto failure = enqueue_edit_locked(
        state_, environment_,
        {.kind = HubEditCommand::Kind::begin, .id = id, .window = window,
         .payload = std::move(payload)});
    if (!failure) return id;
    listener = fail_edit_locked(*state_, std::move(*failure));
  }
  if (listener) listener();
  return id;
}

void DaemonHub::edit_live_profile(const std::uint64_t session,
                                  std::string profile_json) {
  {
    // Consecutive documents coalesce: only the newest is sent.
    std::lock_guard lock(state_->mutex);
    if (session == 0 || session != state_->edit_current) return;
    auto &queued = state_->edit_commands;
    if (!queued.empty() && queued.back().kind == HubEditCommand::Kind::live_profile &&
        queued.back().id == session) {
      queued.back().payload.profile_json = std::move(profile_json);
      return;
    }
  }
  HubEditCommand command{.kind = HubEditCommand::Kind::live_profile, .id = session};
  command.payload.profile_json = std::move(profile_json);
  send_edit(state_, environment_, std::move(command));
}

void DaemonHub::edit_anchor(const std::uint64_t session, std::string panel_id,
                            const double x, const double y, const bool final) {
  HubEditCommand command{.kind = HubEditCommand::Kind::anchor, .id = session,
                         .panel_id = std::move(panel_id), .x = x, .y = y,
                         .flag = final};
  {
    // An unsent drag position of the same bar is replaced by the newer one.
    std::lock_guard lock(state_->mutex);
    if (session == 0 || session != state_->edit_current) return;
    auto &queued = state_->edit_commands;
    if (!queued.empty() && queued.back().kind == HubEditCommand::Kind::anchor &&
        queued.back().id == session && !queued.back().flag &&
        queued.back().panel_id == command.panel_id) {
      queued.back() = std::move(command);
      return;
    }
  }
  send_edit(state_, environment_, std::move(command));
}

void DaemonHub::edit_direct_drag(const std::uint64_t session, const bool enabled) {
  send_edit(state_, environment_,
            {.kind = HubEditCommand::Kind::direct_drag, .id = session, .flag = enabled});
}

void DaemonHub::end_edit(const std::uint64_t session, const bool notify) noexcept {
  try {
    std::lock_guard lifecycle_lock(state_->lifecycle_mutex);
    std::lock_guard lock(state_->mutex);
    if (session == 0 || session != state_->edit_current) return;
    state_->edit_current = 0;
    state_->edit_events.clear();
    try {
      if (!enqueue_edit_locked(state_, environment_,
                              {.kind = HubEditCommand::Kind::end, .id = session, .flag = notify}))
        return;
    } catch (...) {
    }
    state_->unqueued_end.emplace(session, notify);
    (void)SetEvent(state_->wake_event);
  } catch (...) {
  }
}

std::vector<OnWindowEditEvent> DaemonHub::take_edit_events(const std::uint64_t session) {
  std::lock_guard lock(state_->mutex);
  std::vector<OnWindowEditEvent> events;
  // A superseded session's events are dropped; the newest session's final
  // event (ended, expired) is still delivered after the hub retired it.
  if (session == 0 || session != state_->next_edit_id) return events;
  events.swap(state_->edit_events);
  return events;
}

void DaemonHub::set_edit_listener(std::function<void()> listener) {
  std::lock_guard lock(state_->mutex);
  state_->edit_listener = std::move(listener);
}

} // namespace layerc::editor
