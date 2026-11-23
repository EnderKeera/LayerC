// F01: the live-edit controller owns the daemon protocol lifecycle. A scripted
// transport, an injected clock and an injected target predicate keep every
// protocol rule testable without a daemon, a pipe or a real window.
#include "editor_live_edit_test_access.hpp"
#include "live_edit_session.hpp"
#include "test_support.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace core = layerc::core;
namespace editor = layerc::editor;
using layerc::test::expect;

const HWND target = reinterpret_cast<HWND>(std::uintptr_t{0x51});
const HWND second_target = reinterpret_cast<HWND>(std::uintptr_t{0x52});

struct Fixture {
  editor::FakeLiveEditTransport *transport{};
  editor::LiveEditSession session;
  ULONGLONG now{1000};
  bool usable{true};

  Fixture() {
    auto owned = std::make_unique<editor::FakeLiveEditTransport>();
    transport = owned.get();
    session = editor::LiveEditSession(
        std::move(owned), [this] { return now; },
        [this](HWND) { return usable; });
  }

  void receive(const core::LiveMessage &message) {
    transport->inbox.push_back(message);
  }
  void receive(core::LiveMessageType type, std::string text = {},
               core::EditSessionId edit_session = 0,
               core::ProfileRevision revision = 0) {
    core::LiveMessage message;
    message.type = type;
    message.revision = revision;
    message.edit_session = edit_session;
    message.text = std::move(text);
    receive(message);
  }
  void authenticate() {
    receive(core::LiveMessageType::acknowledgement,
            core::package_identity("LayerD") + ";handshake");
    (void)session.poll({});
  }
  [[nodiscard]] std::vector<core::LiveMessage>
  sent(const core::LiveMessageType type) const {
    return transport->sent_of(type);
  }
  [[nodiscard]] std::size_t sent_count() const { return transport->sent.size(); }
};

core::LiveMessage anchor(const core::ProfileRevision revision,
                         core::EditSessionId session, std::string panel,
                         const bool final = false) {
  core::LiveMessage message;
  message.type = core::LiveMessageType::anchor_changed;
  message.revision = revision;
  message.edit_session = session;
  message.panel_id = std::move(panel);
  message.anchor_x = 0.25;
  message.anchor_y = 0.75;
  message.final = final;
  return message;
}

void handshake_and_authentication() {
  Fixture fixture;
  const auto step = fixture.session.poll({});
  expect(fixture.sent(core::LiveMessageType::handshake).size() == 1 &&
             fixture.session.connected() && !fixture.session.authenticated() &&
             step.authenticating && !step.authenticated_now,
         "a connection change authenticates the package with one handshake");
  expect(!fixture.session.poll({}).authenticating,
         "the authenticating notice belongs to the handshake poll only");

  fixture.receive(core::LiveMessageType::acknowledgement, "other-package;1");
  auto rejected = fixture.session.poll({});
  expect(rejected.authentication_rejected && !fixture.session.authenticated() &&
             !rejected.authenticating,
         "an acknowledgement from another package is rejected, not accepted");
  expect(!fixture.session.poll({}).authenticating,
         "a rejected acknowledgement does not re-announce authentication");

  fixture.receive(core::LiveMessageType::acknowledgement,
                  core::package_identity("LayerD") + ";1");
  auto accepted = fixture.session.poll({});
  expect(accepted.authenticated_now && fixture.session.authenticated(),
         "the matching package acknowledgement authenticates the session");
  (void)rejected;
}

void pending_target_and_disconnect() {
  Fixture fixture;
  (void)fixture.session.poll({});
  expect(fixture.session.request_begin(
             target, {.profile_key = "key", .panel_id = "p", .profile_json = {}}, 2000) ==
             editor::LiveEditBegin::awaiting_daemon &&
             fixture.session.awaiting_target(),
         "an unauthenticated begin awaits the daemon instead of starting");

  fixture.transport->online = false;
  fixture.transport->changed = true;
  auto disconnected = fixture.session.poll({});
  expect(!disconnected.authenticating &&
             !disconnected.begin_ready &&
             fixture.sent(core::LiveMessageType::begin_edit).empty(),
         "a disconnect cannot begin the awaited session");
  expect(fixture.session.awaiting_target(),
         "a disconnect keeps the awaited target for its deadline");

  fixture.now += 2000;
  auto expired = fixture.session.poll({});
  expect(expired.pending_expired && !fixture.session.awaiting_target(),
         "the awaited target expires at its deadline");
  expect(!fixture.session.poll({}).pending_expired,
         "the expiry is reported once");

  fixture.transport->online = true;
  fixture.transport->changed = true;
  (void)fixture.session.poll({});
  expect(fixture.session.request_begin(
             second_target, {.profile_key = "key", .panel_id = "p", .profile_json = {}}, 2000) ==
             editor::LiveEditBegin::awaiting_daemon,
         "a later begin before authentication awaits the daemon again");
  fixture.receive(core::LiveMessageType::acknowledgement,
                  core::package_identity("LayerD") + ";1");
  auto ready = fixture.session.poll({});
  expect(ready.authenticated_now &&
             ready.begin_ready == std::optional<HWND>{second_target} &&
             !fixture.session.awaiting_target(),
         "authentication releases the awaited target as begin_ready");
}

void session_lifecycle_and_reconnect() {
  Fixture fixture;
  (void)fixture.session.poll({});
  fixture.authenticate();
  expect(fixture.session.request_begin(
             target, {.profile_key = "key", .panel_id = "p", .profile_json = {}}, 2000) ==
                 editor::LiveEditBegin::started &&
             fixture.session.editing() &&
             fixture.sent(core::LiveMessageType::begin_edit).size() == 1,
         "an authenticated begin starts the session and sends begin_edit");

  // A reconnect inside one poll interval leaves no session behind.
  fixture.transport->online = false;
  fixture.transport->changed = true;
  fixture.transport->online = true;
  auto reconnected = fixture.session.poll({});
  expect(reconnected.ended &&
             reconnected.ended->reason == editor::LiveEditEndReason::connection_lost &&
             !fixture.session.editing(),
         "a reconnect inside one poll interval ends the stale session");

  (void)fixture.session.poll({});
  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  fixture.transport->online = false;
  fixture.transport->changed = true;
  auto lost = fixture.session.poll({});
  expect(lost.ended && lost.ended->reason == editor::LiveEditEndReason::connection_lost &&
             !fixture.session.editing() && fixture.session.target() == nullptr,
         "an active-session disconnect reports connection_lost");

  fixture.transport->online = true;
  fixture.transport->changed = true;
  auto reauth = fixture.session.poll({});
  expect(!fixture.session.authenticated() && !reauth.ended,
         "a bare reconnect re-handshakes and starts unauthenticated");
}

void anchor_ordering() {
  Fixture fixture;
  (void)fixture.session.poll({});
  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  const auto session = fixture.session.session_id();

  fixture.receive(anchor(5, session, "p"));
  auto first = fixture.session.poll([&](std::string_view panel) {
    return panel == "p";
  });
  expect(first.anchors.size() == 1 && first.anchors[0].panel_id == "p" &&
             fixture.session.peer_revision() == 5,
         "an accepted anchor advances the peer revision");

  fixture.receive(anchor(5, session, "p"));
  auto stale = fixture.session.poll([&](std::string_view) { return true; });
  expect(stale.anchors.empty() && fixture.session.peer_revision() == 5,
         "a stale peer revision is ignored");

  fixture.receive(anchor(6, session, "unknown"));
  auto unknown = fixture.session.poll([&](std::string_view panel) {
    return panel == "p";
  });
  expect(unknown.anchors.empty() && fixture.session.peer_revision() == 5,
         "an unknown panel never advances the peer revision");

  fixture.receive(anchor(6, session, "p", true));
  auto accepted = fixture.session.poll([&](std::string_view) { return true; });
  expect(accepted.anchors.size() == 1 && accepted.anchors[0].final &&
             fixture.session.peer_revision() == 6,
         "the next accepted anchor advances the peer revision again");

  fixture.receive(anchor(7, session, "p", true));
  fixture.receive(anchor(8, session, "p"));
  auto both = fixture.session.poll([&](std::string_view) { return true; });
  expect(both.anchors.size() == 2 && both.anchors[0].final &&
             !both.anchors[1].final && fixture.session.peer_revision() == 8,
         "a final followed by a new drag in one poll keeps both, in order");
}

// LayerD ends Direct drag itself after losing the pointer capture; the edit
// session continues and the end shares the anchor revision order.
void daemon_ends_direct_drag() {
  Fixture fixture;
  (void)fixture.session.poll({});
  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  const auto session = fixture.session.session_id();
  const auto direct_drag = [&](const core::ProfileRevision revision,
                               const core::EditSessionId id, const bool enabled) {
    core::LiveMessage message;
    message.type = core::LiveMessageType::set_direct_drag;
    message.revision = revision;
    message.edit_session = id;
    message.direct_drag = enabled;
    return message;
  };
  const auto any_panel = [](std::string_view) { return true; };

  fixture.receive(anchor(5, session, "p"));
  (void)fixture.session.poll(any_panel);
  fixture.receive(direct_drag(6, session, false));
  auto ended = fixture.session.poll(any_panel);
  expect(ended.direct_drag_ended && !ended.ended && ended.forwarded.empty() &&
             fixture.session.editing() && fixture.session.peer_revision() == 6,
         "LayerD ending Direct drag keeps the session and advances the peer revision");

  fixture.receive(direct_drag(6, session, false));
  fixture.receive(direct_drag(7, session + 1, false));
  fixture.receive(direct_drag(8, session, true));
  auto ignored = fixture.session.poll(any_panel);
  expect(!ignored.direct_drag_ended && fixture.session.peer_revision() == 6,
         "a stale, foreign or enabling Direct drag message is ignored");
}

void end_and_pending_sends() {
  Fixture fixture;
  fixture.session.seed_revision(7);
  (void)fixture.session.poll({});
  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  const auto session = fixture.session.session_id();

  fixture.session.request_live_profile();
  auto due = fixture.session.poll({});
  expect(!due.live_profile_due,
         "a coalesced live profile is not due before its interval");
  fixture.now += editor::LiveEditSession::live_profile_interval_ms;
  due = fixture.session.poll({});
  expect(due.live_profile_due && !fixture.session.live_profile_pending(),
         "a coalesced live profile is reported once when due");

  expect(fixture.session.send_anchor("p", 0.5, 0.5, false),
         "the first drag anchor is sent");
  expect(!fixture.session.send_anchor("p", 0.6, 0.5, false),
         "a drag anchor inside one frame is throttled");
  expect(fixture.session.send_anchor("p", 0.6, 0.5, true),
         "a final anchor is never throttled");
  const auto anchors = fixture.sent(core::LiveMessageType::anchor_changed);
  expect(anchors.size() == 2 &&
             anchors.front().revision < anchors.back().revision &&
             anchors.back().final,
         "accepted anchors are stamped with increasing revisions");

  fixture.session.request_live_profile();
  const auto before_end = fixture.sent_count();
  expect(fixture.session.end(true) && !fixture.session.editing(),
         "ending an active session reports that it was active");
  expect(fixture.sent_count() == before_end + 1 &&
             fixture.sent(core::LiveMessageType::end_edit).size() == 1 &&
             fixture.sent(core::LiveMessageType::end_edit).front().edit_session ==
                 session,
         "end_edit is sent only for the notified, active session");
  fixture.now += editor::LiveEditSession::live_profile_interval_ms;
  auto after_end = fixture.session.poll({});
  expect(!after_end.live_profile_due && !fixture.session.live_profile_pending() &&
             !fixture.session.send_anchor("p", 0.7, 0.7, true) &&
             !fixture.session.send_live_profile("{}"),
         "an ended session never sends its pending profile or anchors again");

  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  const auto before_silent_end = fixture.sent_count();
  expect(fixture.session.end(false),
         "a silent end still retires the session");
  expect(fixture.sent_count() == before_silent_end,
         "end_edit is not sent when the caller does not notify the daemon");
}

void heartbeat_cadence_and_target_loss() {
  Fixture fixture;
  (void)fixture.session.poll({});
  fixture.authenticate();
  (void)fixture.session.request_begin(target, {}, 2000);
  auto step = fixture.session.poll({});
  expect(step.timer_interval && *step.timer_interval == 16 &&
             fixture.session.timer_interval() == 16,
         "an active session moves to the frame-rate poll cadence");
  expect(fixture.sent(core::LiveMessageType::heartbeat).empty(),
         "no heartbeat is sent before the interval elapses");
  fixture.now += editor::LiveEditSession::heartbeat_interval_ms - 1;
  (void)fixture.session.poll({});
  expect(fixture.sent(core::LiveMessageType::heartbeat).empty(),
         "a heartbeat inside the interval is skipped");
  fixture.now += 1;
  (void)fixture.session.poll({});
  expect(fixture.sent(core::LiveMessageType::heartbeat).size() == 1,
         "the heartbeat resumes at the 250 ms interval");

  fixture.usable = false;
  fixture.now += editor::LiveEditSession::heartbeat_interval_ms;
  auto closed = fixture.session.poll({});
  expect(closed.ended &&
             closed.ended->reason == editor::LiveEditEndReason::target_closed &&
             !fixture.session.editing(),
         "a target that fails the predicate ends the session as closed");

  fixture.transport->online = false;
  fixture.transport->changed = true;
  (void)fixture.session.poll({});
  expect(fixture.session.timer_interval() == 100,
         "an idle connection drops back to the reconnect cadence");
}

void committed_publish_and_forwarding() {
  Fixture fixture;
  (void)fixture.session.poll({});
  const auto start = fixture.session.revision();
  expect(!fixture.session.publish_committed("key", "{}") &&
             fixture.session.revision() == start + 1,
         "a commit while disconnected advances the revision without sending");

  fixture.authenticate();
  const auto before = fixture.session.revision();
  expect(fixture.session.publish_committed("key", "{}") &&
             fixture.session.revision() == before + 1 &&
             fixture.sent(core::LiveMessageType::committed_profile).size() == 1,
         "an authenticated commit is published with a new revision");

  fixture.receive(core::LiveMessageType::runtime_status, "status");
  fixture.receive(core::LiveMessageType::error, "unsolicited", 99);
  core::LiveMessage acknowledgement;
  acknowledgement.type = core::LiveMessageType::acknowledgement;
  acknowledgement.revision = fixture.session.revision();
  acknowledgement.text = "committed profile applied";
  fixture.receive(acknowledgement);
  const auto step = fixture.session.poll({});
  expect(step.forwarded.size() == 3 &&
              step.forwarded.front().type == core::LiveMessageType::runtime_status &&
              step.forwarded[1].type == core::LiveMessageType::error &&
              step.forwarded.back().type == core::LiveMessageType::acknowledgement &&
              step.forwarded.back().revision == acknowledgement.revision,
          "messages this session does not own, including acknowledgements, are forwarded in order");

  expect(!fixture.session.send_in_session({}) ,
         "no session-scoped send exists without an active session");
  (void)fixture.session.request_begin(target, {}, 2000);
  const auto session = fixture.session.session_id();
  expect(fixture.session.send_in_session(
             {.type = core::LiveMessageType::select_panel, .panel_id = "p"}) &&
             fixture.sent(core::LiveMessageType::select_panel).front().edit_session ==
                 session,
         "a session-scoped send is stamped with the session and revision");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(handshake_and_authentication),
                 LAYERC_TEST_CASE(pending_target_and_disconnect),
                 LAYERC_TEST_CASE(session_lifecycle_and_reconnect),
                 LAYERC_TEST_CASE(anchor_ordering),
                 LAYERC_TEST_CASE(daemon_ends_direct_drag),
                 LAYERC_TEST_CASE(end_and_pending_sends),
                 LAYERC_TEST_CASE(heartbeat_cadence_and_target_loss),
                 LAYERC_TEST_CASE(committed_publish_and_forwarding))
