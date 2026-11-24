#include "daemon_owner_tests.hpp"

#include "daemon_state.hpp"
#include "test_support.hpp"

#include "layerc/core/live_edit.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/live_channel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

// Reaches the private presentation-timer owner with a caller-supplied
// message-only window; no transport, package resource or input is involved.
struct CompanionHostTestAccess {
  static void schedule(CompanionHost &host,
                       const std::optional<layerc::core::TimePoint> next,
                       const layerc::core::TimePoint now) {
    host.schedule_publication(next, now);
  }
};

} // namespace layerc::daemon

namespace layerc::test::daemon_owner {
namespace {

using namespace layerc::daemon;
using layerc::runtime::IRuntimeService;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileStatus;

static_assert(std::is_base_of_v<IRuntimeService, InputRuntime>);
static_assert(std::is_base_of_v<IRuntimeService, HidRuntime>);
static_assert(std::is_base_of_v<IRuntimeService, VisualCaptureRuntime>);
static_assert(std::is_base_of_v<IRuntimeService, IsolatedVisualCaptureService>);
static_assert(std::is_base_of_v<IRuntimeService, AutomationRuntimeHost>);
static_assert(std::is_base_of_v<IRuntimeService, OverlayRuntime>);
static_assert(std::is_base_of_v<IRuntimeService, LiveEditHost>);
static_assert(std::is_base_of_v<IRuntimeService, CompanionHost>);

std::wstring disposable_pipe_name() {
  static unsigned sequence{};
  return L"\\\\.\\pipe\\LayerC.OwnerTest." +
         std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(++sequence);
}

// Real service owners, independently composed with narrow fake ports.
// No DaemonApp, product store, production pipe, input synthesis, or package
// transport is used. Binding references to later peers is inert; service calls
// begin only after this complete graph has been constructed.
class OwnerFixture final : public SessionControl,
                           public SessionQueries,
                           public RuntimeNotifications,
                           public RuntimeDiagnosticsSource,
                           public OverlayEventSink {
public:
  explicit OwnerFixture(std::wstring pipe = disposable_pipe_name(),
                        const DWORD expected_editor = 0)
      : live(sessions, *this, overlays, captures, automation, *this, *this,
             std::move(pipe), expected_editor),
        overlays(*this, sessions, live, window, *this, companion, captures),
        input(sessions, window, hid, automation, overlays, *this),
        hid(sessions, window, input, automation, *this),
        captures(sessions, live, overlays, automation, *this),
        isolated(captures), automation(sessions, live, overlays, window),
        companion(sessions, *this, *this, window) {}

  ~OwnerFixture() {
    live.quiesce();
    automation.quiesce();
    isolated.quiesce();
    captures.quiesce();
    overlays.quiesce();
    input.quiesce();
    hid.quiesce();
    companion.quiesce();
  }

  void reload_profiles() override { ++reloads; }
  void apply_profile_update(std::string, layerc::core::GameProfile,
                            layerc::core::ProfileRevision) override {
    ++updates;
  }
  void refresh_game_windows() override { ++refreshes; }
  [[nodiscard]] std::vector<Session *> foreground_sessions() const override {
    return foreground;
  }
  void sync_foreground_state() override { ++foreground_syncs; }
  void reconcile_runtime_services() override { ++reconciliations; }
  void update_tooltip(const std::wstring &value) override { tooltip = value; }
  [[nodiscard]] std::string runtime_diagnostics_json() const override {
    return "{\"ownerTest\":true}";
  }
  void on_overlay_event(DWORD, HWND) override {}

  TempDirectory sandbox{"LayerC-owner-test"};
  HWND window{};
  SessionRegistry sessions;
  LiveEditHost live;
  OverlayRuntime overlays;
  InputRuntime input;
  HidRuntime hid;
  VisualCaptureRuntime captures;
  IsolatedVisualCaptureService isolated;
  AutomationRuntimeHost automation;
  CompanionHost companion;

  std::vector<Session *> foreground;
  std::wstring tooltip;
  unsigned reloads{};
  unsigned updates{};
  unsigned refreshes{};
  unsigned foreground_syncs{};
  unsigned reconciliations{};
};

layerc::core::GameProfile input_profile() {
  layerc::core::GameProfile profile;
  profile.id = "owner-test";
  profile.game.global = true;
  layerc::core::CooldownDefinition cooldown;
  cooldown.id = "spell";
  cooldown.name = "Spell";
  cooldown.duration = layerc::core::Milliseconds{1000};
  profile.cooldowns.push_back(std::move(cooldown));
  layerc::core::InputBinding binding;
  binding.cooldown_id = "spell";
  binding.chord.tokens.push_back({layerc::core::InputDevice::keyboard, 0x1E});
  profile.bindings.push_back(std::move(binding));
  return profile;
}

Session &add_session(OwnerFixture &fixture,
                     layerc::core::GameProfile profile) {
  SessionInit init;
  init.profile = std::move(profile);
  init.source_key = "owner-source";
  fixture.sessions.sessions.push_back(
      std::make_unique<Session>(std::move(init)));
  return *fixture.sessions.sessions.back();
}

} // namespace

void input_owner_dispatches_without_a_daemon_app() {
  OwnerFixture fixture;
  auto &session = add_session(fixture, input_profile());
  session.clear_input_state();
  fixture.sessions.active_inputs.push_back(&session);
  fixture.input.process_input(
      session, {{layerc::core::InputDevice::keyboard, 0x1E}, true});
  const auto states =
      session.automation.snapshot(layerc::core::MonotonicClock::now());
  expect(states.size() == 1 && states.front().available_charges == 0,
         "the input owner dispatches the binding to its session engine");
  expect(session.last_input_match == "keyboard:30:press" &&
             fixture.tooltip == L"LayerD - triggered spell",
         "input match and trigger notification belong to the owner");
  expect(fixture.input.resources() == RuntimeResourceStats{},
         "code-authored input dispatch does not register or synthesize input");
  fixture.input.process_input(
      session, {{layerc::core::InputDevice::keyboard, 0x1E}, false});
  expect(session.last_input_match == "keyboard:30:press" &&
             session.automation.snapshot(
                 layerc::core::MonotonicClock::now()).front().available_charges ==
                 0,
         "release does not synthesize a second trigger or erase the last match");
}

void dormant_owners_do_not_recreate_resources() {
  OwnerFixture fixture;
  expect(fixture.captures.reconcile({}).status == ServiceReconcileStatus::ok &&
             fixture.isolated.reconcile({}).status ==
                 ServiceReconcileStatus::ok,
         "capture owners accept empty demand without creating a worker");
  fixture.captures.poll();
  fixture.captures.maintain();
  fixture.captures.retry();
  expect(fixture.captures.captures().empty() &&
             fixture.captures.snapshot_event() == nullptr &&
             !fixture.captures.needs_maintenance(),
         "capture polling and retry are inert when there are no consumers");
  fixture.captures.quiesce();
  fixture.isolated.quiesce();
  fixture.overlays.quiesce();
  fixture.overlays.mark_dirty();
  fixture.overlays.flush();
  fixture.companion.quiesce();
  fixture.companion.publish_snapshot(layerc::core::MonotonicClock::now());
  fixture.companion.poll();
  fixture.input.quiesce();
  fixture.hid.quiesce();
  fixture.automation.quiesce();

  expect(fixture.captures.resources() == RuntimeResourceStats{} &&
             fixture.isolated.resources() == RuntimeResourceStats{} &&
             fixture.overlays.resources() == RuntimeResourceStats{} &&
             fixture.companion.resources() == RuntimeResourceStats{} &&
             fixture.companion.resources().timers == 0 &&
             fixture.input.resources() == RuntimeResourceStats{} &&
             fixture.hid.resources() == RuntimeResourceStats{} &&
             fixture.automation.resources() == RuntimeResourceStats{},
         "quiesced owners release resources and a late dirty flush stays idle");
  expect(fixture.live.edit_session() == nullptr &&
             !fixture.live.authenticated() &&
             !fixture.live.send_authenticated(
                 {.type = layerc::core::LiveMessageType::runtime_status}),
         "the control owner cannot publish to an unauthenticated peer");
}

void automation_owner_controls_runtime_and_trace() {
  OwnerFixture fixture;
  auto base = input_profile();
  layerc::core::AutomationProfile profile;
  profile.profile_id = base.id;
  profile.variables.push_back(
      {.id = "started",
       .type = layerc::core::AutomationValueType::boolean,
       .default_value = layerc::core::AutomationValue{false}});
  layerc::core::AutomationDefinition definition;
  definition.id = "startup";
  definition.name = "Startup";
  layerc::core::AutomationRule rule;
  rule.id = "started";
  rule.event = layerc::core::AutomationEventKind::session_started;
  layerc::core::AutomationAction action;
  action.kind = layerc::core::AutomationActionKind::set_variable;
  action.target = "started";
  action.value = layerc::core::expr::literal(
      layerc::core::AutomationValue{true});
  rule.actions.push_back(std::move(action));
  definition.rules.push_back(std::move(rule));
  profile.automations.push_back(std::move(definition));

  ProfileSource source;
  source.key = "owner-source";
  source.path = fixture.sandbox.path() / L"owner.json";
  source.profile = base;
  source.modules.get<layerc::core::AutomationModule>().profile = profile;
  source.modules.get<layerc::core::AutomationModule>().mark_edited();
  auto init = source.to_session_init(false);
  fixture.sessions.sources.push_back(std::move(source));
  fixture.sessions.sessions.push_back(
      std::make_unique<Session>(std::move(init)));
  auto &session = *fixture.sessions.sessions.back();

  fixture.automation.set_trace_subscribed(true);
  expect(fixture.automation.reconcile({}).status ==
             ServiceReconcileStatus::ok &&
             session.automation.runtime() != nullptr,
         "the automation owner starts its matching session runtime");
  if (session.automation.runtime() != nullptr) {
    const auto &variables = session.automation.runtime()->snapshot().variables;
    const auto started = variables.find("started");
    expect(started != variables.end() &&
               started->second == layerc::core::AutomationValue{true},
           "the owner evaluates session-started effects without the shell");
    expect(fixture.automation.retry("owner-source", "started"),
           "automation retry is an owner operation");
  }
  expect(!fixture.automation.retry("other-source", "started"),
         "an unrelated source cannot retry this owner session");
  fixture.automation.set_trace_subscribed(false);
  expect(!fixture.automation.trace_subscribed(),
         "trace subscription is controlled and cleared by its owner");
  fixture.automation.quiesce();
  expect(session.automation.runtime() == nullptr &&
             fixture.automation.resources() == RuntimeResourceStats{},
         "quiescence stops runtimes and releases the owner's active resources");
}

void live_owner_owns_control_resources_and_faults() {
  {
    OwnerFixture fixture;
    expect(fixture.live.reconcile({}).status == ServiceReconcileStatus::ok,
           "the mandatory control owner starts its disposable pipe");
    expect(fixture.live.resources().handles == 4,
           "the control owner accounts the pipe and three I/O events");
    std::array<HANDLE, 3> handles{};
    expect(fixture.live.wait_handles(handles) != 0,
           "the owner exposes its pending control wait handle");
    fixture.live.poll();
    expect(!fixture.live.authenticated() &&
               fixture.live.edit_session() == nullptr &&
               fixture.updates == 0 && fixture.reloads == 0,
           "a listening pipe alone cannot authenticate or mutate profiles");
    fixture.live.quiesce();
    expect(fixture.live.resources() == RuntimeResourceStats{} &&
               fixture.live.wait_handles(handles) == 0 &&
               fixture.live.peer_process_id() == 0,
           "control quiescence drains I/O and releases every owned handle");
    fixture.live.poll();
    expect(fixture.live.resources() == RuntimeResourceStats{},
           "a late poll cannot restart a quiesced control owner");
    expect(fixture.live.reconcile({}).status == ServiceReconcileStatus::ok,
           "an explicit reconcile can restart the stopped owner");
  }
  {
    OwnerFixture fixture(L"not-a-named-pipe");
    const auto result = fixture.live.reconcile({});
    expect(result.status == ServiceReconcileStatus::failed &&
               !result.reason.empty() &&
               fixture.live.resources() == RuntimeResourceStats{},
           "control startup faults stay visible and release partial resources");
  }
}

// layer57 L57-01: LayerD answers an editor's Direct drag off request with the
// request's session and revision once it is applied. An on request and a
// stale off get no such answer, so an editor never takes either as confirmed.
void live_owner_confirms_direct_drag_off() {
  using layerc::core::LiveMessage;
  using Type = layerc::core::LiveMessageType;
  using layerc::platform::NamedPipeEditorDaemonChannel;
  constexpr layerc::core::EditSessionId id = 7;
  const auto pipe = disposable_pipe_name();
  // Both ends trust this test process instead of the LayerD/LayerE images.
  OwnerFixture fixture(pipe, GetCurrentProcessId());
  if (!expect(fixture.live.reconcile({}).status == ServiceReconcileStatus::ok,
              "the control owner listens on its disposable pipe"))
    return;
  // The channel holds its 64 KiB read buffer inline; keep it off the case's
  // stack so the frame stays bounded under /analyze (C6262).
  const auto channel = std::make_unique<NamedPipeEditorDaemonChannel>(
      NamedPipeEditorDaemonChannel::Role::client, pipe, GetCurrentProcessId());
  auto &client = *channel;
  std::vector<LiveMessage> received;
  const auto pump = [&](const auto &done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
      fixture.live.poll();
      for (auto &result : client.poll())
        if (auto *message = std::get_if<LiveMessage>(&result))
          received.push_back(std::move(*message));
      if (done() || std::chrono::steady_clock::now() >= deadline) return done();
      Sleep(1);
    }
  };
  const auto replies = [&](const Type type, const layerc::core::ProfileRevision revision,
                           const layerc::core::EditSessionId session) {
    return std::ranges::count_if(received, [&](const LiveMessage &message) {
      return message.type == type && message.revision == revision &&
             message.edit_session == session;
    });
  };
  const auto drag = [&](const layerc::core::ProfileRevision revision, const bool enabled) {
    LiveMessage message;
    message.type = Type::set_direct_drag;
    message.revision = revision;
    message.edit_session = id;
    message.direct_drag = enabled;
    return client.send(message);
  };
  const auto direct_drag_on = [&] {
    return fixture.live.edit_session() != nullptr && fixture.live.edit_session()->direct_drag;
  };

  LiveMessage handshake;
  handshake.revision = 1;
  handshake.text = layerc::core::package_identity("LayerE");
  if (!expect(pump([&] { return client.connected() && fixture.live.connected(); }),
              "an editor connects to the control owner") ||
      !expect(client.send(handshake) &&
                  pump([&] { return replies(Type::acknowledgement, 1, 0) == 1; }) &&
                  fixture.live.authenticated(),
              "the editor authenticates"))
    return;

  // The smallest valid profile: one cooldown, its key and its bar. The bars
  // live only in this test process and close when the session ends.
  LiveMessage begin;
  begin.type = Type::begin_edit;
  begin.revision = 2;
  begin.edit_session = id;
  begin.target_window = reinterpret_cast<std::uintptr_t>(GetDesktopWindow());
  begin.profile_key = "owner-source";
  layerc::core::GameProfile profile;
  profile.id = "owner-edit";
  profile.name = "Owner edit";
  profile.game.executable_name = "owner-edit.exe";
  profile.cooldowns.push_back({.id = "spell", .name = "Spell",
                               .duration = layerc::core::Milliseconds{1000}});
  profile.panels.push_back({.id = "spell-bar", .name = "Spell",
                            .style = layerc::core::PanelStyle::bar,
                            .cooldown_ids = {"spell"}});
  profile.bindings.push_back(
      {.cooldown_id = "spell",
       .chord = {{{layerc::core::InputDevice::keyboard, 0x1E, 0}}}});
  begin.profile_json = layerc::core::serialize_profile_json(profile);
  if (!expect(client.send(begin) &&
                  pump([&] { return replies(Type::acknowledgement, 2, id) == 1; }) &&
                  fixture.live.edit_session() != nullptr,
              "LayerD starts the edit session"))
    return;

  expect(drag(3, true) && pump(direct_drag_on), "Direct drag turns on");
  expect(drag(4, false) && pump([&] { return replies(Type::acknowledgement, 4, id) == 1; }),
         "LayerD answers the off request");
  const auto answer = std::ranges::find_if(received, [](const LiveMessage &message) {
    return message.type == Type::acknowledgement && message.revision == 4;
  });
  expect(answer != received.end() && answer->text == layerc::core::live_direct_drag_off_ack &&
             fixture.live.edit_session() != nullptr && !direct_drag_on() &&
             replies(Type::acknowledgement, 3, id) == 0,
         "the answer names the applied off; the on request had none");

  expect(drag(3, false) && pump([&] { return replies(Type::error, 3, id) == 1; }) &&
             replies(Type::acknowledgement, 3, id) == 0,
         "a stale off is refused, never confirmed");

  LiveMessage end;
  end.type = Type::end_edit;
  end.revision = 5;
  end.edit_session = id;
  expect(client.send(end) && pump([&] { return fixture.live.edit_session() == nullptr; }),
         "the editor ends the session");
}

void companion_owner_timer_lifecycle() {
  OwnerFixture fixture;
  const HWND messages = CreateWindowExW(0, L"STATIC", L"LayerC owner timer", 0, 0,
                                        0, 0, 0, HWND_MESSAGE, nullptr, nullptr,
                                        nullptr);
  if (!expect(messages != nullptr,
              "the owner timer fixture creates a message-only window"))
    return;
  // CompanionHost holds a reference to the fixture's window member.
  fixture.window = messages;
  const auto now = layerc::core::MonotonicClock::now();
  const auto first = now + layerc::core::Milliseconds{125};
  layerc::daemon::CompanionHostTestAccess::schedule(fixture.companion, first, now);
  expect(fixture.companion.resources().timers == 1,
         "a 125 ms scheduled publication owns exactly one timer");
  layerc::daemon::CompanionHostTestAccess::schedule(
      fixture.companion, now + layerc::core::Milliseconds{250}, now);
  expect(fixture.companion.resources().timers == 1,
         "replacing the deadline still owns exactly one timer");
  layerc::daemon::CompanionHostTestAccess::schedule(fixture.companion, std::nullopt,
                                                    now);
  expect(fixture.companion.resources().timers == 0,
         "a null deadline cancels the owner timer");
  layerc::daemon::CompanionHostTestAccess::schedule(fixture.companion, first, now);
  expect(fixture.companion.resources().timers == 1,
         "a new deadline re-arms the single owner timer");
  fixture.overlays.quiesce();
  fixture.companion.quiesce();
  expect(fixture.companion.resources().timers == 0 &&
             fixture.overlays.resources() == RuntimeResourceStats{},
         "quiescence removes the owner timer and keeps native overlays released");
  fixture.companion.publish_snapshot(layerc::core::MonotonicClock::now());
  expect(fixture.companion.resources().timers == 0,
         "a stale publication cannot recreate the quiesced owner timer");
  fixture.window = nullptr;
  DestroyWindow(messages);
}

} // namespace layerc::test::daemon_owner
