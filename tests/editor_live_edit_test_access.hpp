#pragma once

#include "live_edit_session.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace layerc::editor {

// A scripted editor-side transport: tests push daemon messages into `inbox`,
// inspect `sent`, and simulate connection changes. No pipe is created.
class FakeLiveEditTransport final : public LiveEditTransport {
public:
  std::vector<layerc::core::LiveMessageResult> inbox;
  std::vector<layerc::core::LiveMessage> sent;
  bool online{true};
  bool changed{true};
  bool accept_send{true};

  [[nodiscard]] std::vector<layerc::core::LiveMessageResult> poll() override {
    auto drained = std::move(inbox);
    inbox.clear();
    return drained;
  }
  [[nodiscard]] bool take_connection_changed() override {
    const bool value = changed;
    changed = false;
    return value;
  }
  [[nodiscard]] bool connected() const override { return online; }
  [[nodiscard]] bool send(const layerc::core::LiveMessage &message) override {
    if (!accept_send) return false;
    sent.push_back(message);
    return true;
  }

  [[nodiscard]] std::vector<layerc::core::LiveMessage>
  sent_of(const layerc::core::LiveMessageType type) const {
    std::vector<layerc::core::LiveMessage> matches;
    for (const auto &message : sent)
      if (message.type == type) matches.push_back(message);
    return matches;
  }
};

// Test-only session mutations. The production API exposes no way to fabricate
// a session or a revision; these exist so window-level fixtures can drive the
// controller without a daemon.
struct LiveEditSessionTestAccess {
  // Replaces the session's transport, handing the fake to the session.
  static FakeLiveEditTransport &install(LiveEditSession &session) {
    auto owned = std::make_unique<FakeLiveEditTransport>();
    auto *raw = owned.get();
    session = LiveEditSession(std::move(owned));
    return *raw;
  }
  static void begin_session(LiveEditSession &session, const HWND target,
                            const layerc::core::EditSessionId session_id) {
    session.pending_target_ = nullptr;
    session.pending_deadline_ = 0;
    session.target_ = target;
    session.session_id_ = session_id == 0 ? 1 : session_id;
    session.peer_revision_ = 0;
    session.direct_drag_notice_shown_ = false;
  }
  static void clear_session(LiveEditSession &session) {
    session.retire_session();
  }
  [[nodiscard]] static LiveEditTransport *transport_of(LiveEditSession &session) {
    return session.transport_.get();
  }
  static void authenticate(LiveEditSession &session) {
    session.authenticated_ = true;
  }
};

} // namespace layerc::editor
