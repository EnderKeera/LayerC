#include "platform/win32/daemon_hub_edit.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace layerc::editor {
namespace {

using Event = OnWindowEditEvent;

Event::EndReason end_reason(const LiveEditEndReason reason) noexcept {
  switch (reason) {
  case LiveEditEndReason::connection_lost: return Event::EndReason::connection_lost;
  case LiveEditEndReason::daemon_error: return Event::EndReason::daemon_error;
  case LiveEditEndReason::daemon_ended: return Event::EndReason::daemon_ended;
  case LiveEditEndReason::target_closed: return Event::EndReason::target_closed;
  }
  return Event::EndReason::daemon_ended;
}

LiveEditBeginPayload live_payload(const OnWindowEditBegin &payload) {
  return {payload.profile_key, payload.panel_id, payload.profile_json};
}

// Why LayerD may not edit on target, or empty when it may.
std::string target_refusal(const HWND target) {
  DWORD process{};
  if (target == nullptr || IsWindow(target) == FALSE ||
      GetWindowThreadProcessId(target, &process) == 0 || process == 0)
    return "That window closed before editing began.";
  if (process == GetCurrentProcessId())
    return "LayerC Editor cannot attach edit overlays to its own windows.";
  return {};
}

} // namespace

void HubEditDriver::clear() noexcept {
  id_ = 0;
  editing_ = false;
  payload_ = {};
  pending_profile_.reset();
}

void HubEditDriver::finish(Event event) {
  const auto id = id_;
  clear();
  if (id != 0) emit_(id, std::move(event));
}

void HubEditDriver::begin(HubEditCommand command, LiveEditSession *session,
                          const OpenSession &open) {
  // A newer session replaces the older one, which LayerD is told about.
  if (id_ != 0 && session != nullptr) (void)session->end(editing_);
  clear();
  id_ = command.id;
  payload_ = std::move(command.payload);
  const auto target =
      reinterpret_cast<HWND>(static_cast<std::uintptr_t>(command.window));
  if (auto refusal = target_refusal(target); !refusal.empty()) {
    finish({.kind = Event::Kind::ended, .reason = Event::EndReason::unavailable,
            .text = std::move(refusal)});
    return;
  }
  bool started{};
  std::string reason;
  session = open ? open(started, reason) : nullptr;
  if (session == nullptr) {
    finish({.kind = Event::Kind::ended, .reason = Event::EndReason::unavailable,
            .text = reason.empty() ? "The LayerD link is unavailable." : reason});
    return;
  }
  const auto begun = session->request_begin(
      target, live_payload(payload_), started ? await_started_ms : await_running_ms);
  if (begun == LiveEditBegin::started) {
    editing_ = true;
    emit_(id_, {.kind = Event::Kind::started});
    return;
  }
  emit_(id_, {.kind = Event::Kind::awaiting});
}

void HubEditDriver::apply(HubEditCommand command, LiveEditSession *session,
                          const OpenSession &open) {
  if (command.kind == HubEditCommand::Kind::begin) {
    begin(std::move(command), session, open);
    return;
  }
  // Commands for a superseded or finished session are stale.
  if (id_ == 0 || command.id != id_) return;
  switch (command.kind) {
  case HubEditCommand::Kind::live_profile:
    // A session still waiting for LayerD begins from the newest document.
    payload_.profile_json = command.payload.profile_json;
    if (editing_ && session != nullptr) {
      pending_profile_ = std::move(command.payload.profile_json);
      session->request_live_profile();
    }
    return;
  case HubEditCommand::Kind::anchor:
    // A non-final anchor inside one frame of the last is dropped; the
    // release always sends its final position.
    if (editing_ && session != nullptr)
      (void)session->send_anchor(command.panel_id, command.x, command.y, command.flag);
    return;
  case HubEditCommand::Kind::direct_drag:
    if (editing_ && session != nullptr) (void)session->send_direct_drag(command.flag);
    return;
  case HubEditCommand::Kind::end:
    if (session != nullptr) (void)session->end(command.flag && editing_);
    clear();
    return;
  case HubEditCommand::Kind::begin:
    return;
  }
}

void HubEditDriver::apply_step(const LiveEditStep &step, LiveEditSession &session) {
  if (id_ == 0) return;
  for (const auto &error : step.protocol_errors)
    emit_(id_, {.kind = Event::Kind::protocol_error, .text = error.message});
  if (editing_) {
    if (step.session_acknowledged) emit_(id_, {.kind = Event::Kind::acknowledged});
    for (const auto &anchor : step.anchors)
      emit_(id_, {.kind = Event::Kind::anchor, .panel_id = anchor.panel_id,
                  .x = anchor.x, .y = anchor.y, .final = anchor.final});
    if (step.direct_drag_ended) emit_(id_, {.kind = Event::Kind::direct_drag_ended});
    if (step.ended) {
      finish({.kind = Event::Kind::ended, .reason = end_reason(step.ended->reason),
              .text = step.ended->text});
      return;
    }
  } else if (step.begin_ready) {
    // Authenticated now: the begin goes out with the newest document.
    if (session.request_begin(*step.begin_ready, live_payload(payload_), 0) ==
        LiveEditBegin::started) {
      editing_ = true;
      emit_(id_, {.kind = Event::Kind::started});
    }
  } else if (step.pending_expired) {
    finish({.kind = Event::Kind::expired});
    return;
  }
  if (editing_ && step.live_profile_due && pending_profile_) {
    auto profile = std::move(*pending_profile_);
    pending_profile_.reset();
    (void)session.send_live_profile(std::move(profile));
  }
}

void HubEditDriver::connection_discarded(std::string reason) {
  if (id_ == 0) return;
  if (editing_) {
    finish({.kind = Event::Kind::ended, .reason = Event::EndReason::connection_lost});
    return;
  }
  finish({.kind = Event::Kind::ended, .reason = Event::EndReason::unavailable,
          .text = std::move(reason)});
}

DWORD HubEditDriver::wait_timeout(const LiveEditSession &session) const {
  DWORD timeout = INFINITE;
  if (const auto due = session.next_due())
    timeout = static_cast<DWORD>(std::min<ULONGLONG>(*due, INFINITE - 1U));
  // Connecting needs polls: the pipe has no handle to wait on yet.
  if (live() && !session.authenticated()) timeout = std::min(timeout, reconnect_ms);
  return timeout;
}

void HubEditDriver::shutdown(LiveEditSession *session) {
  if (id_ != 0 && session != nullptr) {
    (void)session->end(editing_);
    (void)session->poll({});
  }
  clear();
}

} // namespace layerc::editor
