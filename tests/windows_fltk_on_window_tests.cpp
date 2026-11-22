#include "editor_live_edit_test_access.hpp"
#include "platform/win32/daemon_hub_edit.hpp"
#include "test_support.hpp"

#include "layerc/core/live_edit.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace layerc::editor;
using layerc::core::LiveMessage;
using layerc::core::LiveMessageType;
using layerc::test::expect;
using Event = OnWindowEditEvent;
using Command = HubEditCommand;

struct Fixture {
  ULONGLONG now{1000};
  bool usable{true};
  std::vector<std::pair<std::uint64_t, Event>> events;
  FakeLiveEditTransport *transport{};
  LiveEditSession session{make_transport(), [this] { return now; },
                          [this](HWND) { return usable; }};
  HubEditDriver driver{[this](std::uint64_t id, Event event) {
    events.emplace_back(id, std::move(event));
  }};
  const HubEditDriver::OpenSession open{[this](bool &, std::string &) { return &session; }};

  std::unique_ptr<FakeLiveEditTransport> make_transport() {
    auto result = std::make_unique<FakeLiveEditTransport>();
    transport = result.get();
    return result;
  }
  void authenticate() {
    transport->inbox.emplace_back(LiveMessage{.type = LiveMessageType::acknowledgement,
        .text = layerc::core::package_identity("LayerD") + ";live-profile"});
    poll();
  }
  void poll() { driver.apply_step(session.poll({}), session); }
  void begin(const HWND target = GetDesktopWindow(), const std::uint64_t id = 1) {
    driver.apply({.kind = Command::Kind::begin, .id = id,
                  .window = reinterpret_cast<std::uintptr_t>(target),
                  .payload = {"profile-key", "bar", "first"}, .panel_id = {}}, &session, open);
  }
  void profile(std::string json, const std::uint64_t id = 1) {
    driver.apply({.kind = Command::Kind::live_profile, .id = id,
                  .payload = {{}, {}, std::move(json)}, .panel_id = {}}, &session, {});
  }
  bool last(const Event::Kind kind, const std::uint64_t id = 1) const {
    return !events.empty() && events.back().first == id && events.back().second.kind == kind;
  }
};

void begin_authenticated_and_shutdown() {
  Fixture f;
  f.poll();
  f.authenticate();
  expect(f.driver.wait_timeout(f.session) == INFINITE, "authenticated idle has no deadline");
  f.begin();
  const auto began = f.transport->sent_of(LiveMessageType::begin_edit);
  expect(f.last(Event::Kind::started) && f.driver.editing() && began.size() == 1 &&
             began.front().target_window == reinterpret_cast<std::uintptr_t>(GetDesktopWindow()) &&
             began.front().profile_key == "profile-key" && began.front().panel_id == "bar" &&
             began.front().profile_json == "first", "authenticated begin forwards the full payload");
  f.driver.shutdown(&f.session);
  f.driver.shutdown(&f.session);
  expect(f.transport->sent_of(LiveMessageType::end_edit).size() == 1 &&
             f.driver.wait_timeout(f.session) == INFINITE,
         "shutdown sends end once and clears the deadline");
}

void await_latest_profile_expire_and_refuse() {
  Fixture f;
  f.poll();
  f.begin();
  expect(f.last(Event::Kind::awaiting) && f.driver.wait_timeout(f.session) <= 250,
         "unauthenticated begin retries on a bounded deadline");
  f.profile("obsolete");
  f.profile("newest");
  f.profile("wrong session", 99);
  f.authenticate();
  const auto began = f.transport->sent_of(LiveMessageType::begin_edit);
  expect(f.last(Event::Kind::started) && began.size() == 1 && began.front().profile_json == "newest",
         "pending begin uses only the newest document for its own id");

  Fixture pending;
  pending.poll();
  pending.begin();
  pending.now += HubEditDriver::await_running_ms - 1;
  pending.poll();
  expect(pending.last(Event::Kind::awaiting) && pending.driver.wait_timeout(pending.session) == 1,
         "await does not expire before its monotonic deadline");
  ++pending.now;
  pending.poll();
  expect(pending.last(Event::Kind::expired) && !pending.driver.live() &&
             pending.driver.wait_timeout(pending.session) == INFINITE, "pending deadline expires once");

  const auto own = CreateWindowExW(0, L"STATIC", L"Fixture", WS_POPUP, 0, 0, 10, 10,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!expect(own != nullptr, "create an editor-owned target fixture")) return;
  pending.begin(own, 2);
  expect(pending.last(Event::Kind::ended, 2) &&
             pending.events.back().second.reason == Event::EndReason::unavailable &&
             pending.events.back().second.text == "LayerC Editor cannot attach edit overlays to its own windows.",
         "editor-owned windows are refused before opening a session");
  expect(DestroyWindow(own) != FALSE, "destroy test window");
  pending.begin(own, 3);
  expect(pending.last(Event::Kind::ended, 3) &&
             pending.events.back().second.text == "That window closed before editing began.",
         "closed windows are refused before opening a session");
}

void coalesced_profile_throttled_anchors_and_direct_drag() {
  Fixture f;
  f.poll();
  f.authenticate();
  f.begin();
  f.profile("one");
  f.now += 16;
  f.profile("two");
  f.now += 16;
  f.profile("last");
  f.poll();
  expect(f.transport->sent_of(LiveMessageType::live_profile).empty(), "no publication before 33 ms");
  ++f.now;
  f.poll();
  const auto published = f.transport->sent_of(LiveMessageType::live_profile);
  expect(published.size() == 1 && published.front().profile_json == "last", "latest JSON wins at 33 ms");

  const auto anchor = [&](bool final, double x, std::uint64_t id = 1) {
    f.driver.apply({.kind = Command::Kind::anchor, .id = id, .payload = {}, .panel_id = "bar",
                    .x = x, .y = .5, .flag = final}, &f.session, {});
  };
  anchor(false, .1);
  anchor(false, .2);
  anchor(true, .3, 99);
  f.now += LiveEditSession::drag_throttle_ms;
  anchor(false, .4);
  anchor(true, .5);
  const auto sent = f.transport->sent_of(LiveMessageType::anchor_changed);
  expect(sent.size() == 3 && sent[0].anchor_x == .1 && sent[1].anchor_x == .4 &&
             sent[2].anchor_x == .5 && sent[2].final, "throttle applies only to non-final anchors of the active id");
  f.driver.apply({.kind = Command::Kind::direct_drag, .id = 1,
                  .payload = {}, .panel_id = {}, .flag = true}, &f.session, {});
  const auto toggles = f.transport->sent_of(LiveMessageType::set_direct_drag);
  expect(toggles.size() == 1 && toggles.front().direct_drag, "direct drag reaches the active session");
}

void target_closes_during_edit() {
  Fixture f;
  f.poll();
  f.authenticate();
  f.begin();
  f.usable = false;
  f.now += LiveEditSession::heartbeat_interval_ms;
  f.poll();
  expect(f.last(Event::Kind::ended) &&
             f.events.back().second.reason == Event::EndReason::target_closed &&
             !f.driver.live() && !f.session.editing() &&
             f.driver.wait_timeout(f.session) == INFINITE,
         "losing the active target ends editing and clears the heartbeat deadline");
}

void peer_events_and_discard() {
  for (const auto &[type, reason] : {
      std::pair{LiveMessageType::end_edit, Event::EndReason::daemon_ended},
      std::pair{LiveMessageType::error, Event::EndReason::daemon_error}}) {
    Fixture f;
    f.poll();
    f.authenticate();
    f.begin();
    const auto id = f.session.session_id();
    f.transport->inbox.emplace_back(LiveMessage{.type = LiveMessageType::acknowledgement, .edit_session = id});
    f.transport->inbox.emplace_back(LiveMessage{.type = LiveMessageType::anchor_changed, .revision = 3,
        .edit_session = id, .panel_id = "bar", .anchor_x = .2, .anchor_y = .4});
    f.transport->inbox.emplace_back(LiveMessage{.type = LiveMessageType::anchor_changed, .revision = 2,
        .edit_session = id, .panel_id = "bar", .anchor_x = .1, .anchor_y = .3});
    f.transport->inbox.emplace_back(LiveMessage{.type = LiveMessageType::anchor_changed, .revision = 4,
        .edit_session = id, .panel_id = "bar", .anchor_x = .5, .anchor_y = .6, .final = true});
    f.transport->inbox.emplace_back(LiveMessage{.type = type, .edit_session = id, .text = "peer reason"});
    f.poll();
    expect(f.events.size() == 5 && f.events[1].second.kind == Event::Kind::acknowledged &&
               f.events[2].second.kind == Event::Kind::anchor && f.events[2].second.x == .2 &&
               f.events[3].second.kind == Event::Kind::anchor && f.events[3].second.final &&
               f.events[3].second.y == .6 && f.last(Event::Kind::ended) &&
               f.events.back().second.reason == reason && f.events.back().second.text == "peer reason",
           "acknowledgement, accepted anchors and peer end are delivered in order with text");
  }
  Fixture disconnected;
  disconnected.poll();
  disconnected.authenticate();
  disconnected.begin();
  disconnected.transport->online = false;
  disconnected.transport->changed = true;
  disconnected.poll();
  expect(disconnected.last(Event::Kind::ended) &&
             disconnected.events.back().second.reason == Event::EndReason::connection_lost,
         "connection change ends editing as connection lost");

  Fixture discarded;
  discarded.poll();
  discarded.authenticate();
  discarded.begin();
  discarded.driver.connection_discarded("pipe failure");
  expect(discarded.last(Event::Kind::ended) &&
             discarded.events.back().second.reason == Event::EndReason::connection_lost,
         "discarding an edit connection reports connection lost");
  Fixture awaiting;
  awaiting.poll();
  awaiting.begin();
  awaiting.driver.connection_discarded("authentication failed");
  expect(awaiting.last(Event::Kind::ended) &&
             awaiting.events.back().second.reason == Event::EndReason::unavailable &&
             awaiting.events.back().second.text == "authentication failed",
         "discarding while awaiting preserves the unavailable reason");
}
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(begin_authenticated_and_shutdown),
    LAYERC_TEST_CASE(await_latest_profile_expire_and_refuse),
    LAYERC_TEST_CASE(coalesced_profile_throttled_anchors_and_direct_drag),
    LAYERC_TEST_CASE(target_closes_during_edit),
    LAYERC_TEST_CASE(peer_events_and_discard))
