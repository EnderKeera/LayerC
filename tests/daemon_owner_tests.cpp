#include "daemon_owner_tests.hpp"

#include "daemon_state.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>

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
  explicit OwnerFixture(std::wstring pipe = disposable_pipe_name())
      : live(sessions, *this, overlays, captures, automation, *this, *this,
             std::move(pipe)),
        overlays(*this, sessions, live, window, *this, companion, captures),
        input(sessions, window, hid, automation, overlays, *this),
        hid(sessions, window, input, automation, *this),
        captures(sessions, live, overlays, automation, *this),
        isolated(captures), automation(sessions, live, overlays, window),
        companion(sessions, *this, *this) {}

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

} // namespace layerc::test::daemon_owner
