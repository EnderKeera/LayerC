// Session admission and automation health are independent of an X11 display.
#include "daemon_host.hpp"
#include "trace_lease_host.hpp"
#include "input_service.hpp"
#include "control_channel.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace layerc::core;
using layerc::platform::StoredProfile;

AutomationExpression literal(AutomationValue value) {
  AutomationExpression expression;
  expression.kind = ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

GameProfile base_profile(std::string id) {
  GameProfile profile;
  profile.id = std::move(id);
  profile.name = "Automation only";
  profile.game.global = true;
  return profile;
}

AutomationProfile automation_profile(const GameProfile &base,
                                     const AutomationEventKind event) {
  AutomationProfile profile;
  profile.profile_id = base.id;
  profile.variables = {{.id = "count",
                        .type = AutomationValueType::integer,
                        .default_value = AutomationValue(std::int64_t{0}),
                        .persistent = true}};
  AutomationRule rule;
  rule.id = "increment";
  rule.event = event;
  if (event == AutomationEventKind::input_pressed)
    rule.source_filter = "keyboard:18";
  rule.actions = {{.kind = AutomationActionKind::increment_variable,
                   .target = "count",
                   .name = {},
                   .property = {},
                   .value = literal(AutomationValue(std::int64_t{1})),
                   .secondary = std::nullopt}};
  profile.automations = {{.id = "counter", .name = "Counter", .rules = {rule}}};
  return profile;
}

layerc::platform::WindowScan active_scan() {
  layerc::platform::WindowScan scan;
  layerc::platform::WindowSnapshot window;
  window.window = 0x1234;
  window.active = true;
  scan.windows.push_back(window);
  scan.active = window.window;
  return scan;
}

std::string automation_status_response(
    const layerc::daemon::SessionRegistry &registry) {
  const auto requests = registry.session_requests();
  const auto demand =
      layerc::runtime::calculate_capability_demand(requests, false);
  layerc::daemon::LinuxStatusSnapshot status;
  status.capabilities.push_back(
      {layerc::runtime::Capability::automation,
       layerc::daemon::CapabilityAvailability::inactive, {}, {}});
  const auto diagnostics = registry.automation_diagnostics();
  layerc::daemon::derive_capability_status(
      status, demand,
      {.input = {},
       .overlay = {},
       .automation = std::span<const std::string>{diagnostics},
       .visual = {}});
  const layerc::runtime::DaemonKernel kernel;
  return layerc::daemon::handle_control_request(
             layerc::platform::encode_control_request(
                 layerc::platform::ControlCommand::status),
             kernel, status)
      .response;
}

void test_registry_automation_only_and_health() {
  using layerc::test::expect;
  layerc::test::TempDirectory temp{"layerc-x11-registry"};
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME",
                                           temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  if (!expect(config.applied() && data.applied(), "isolated XDG roots are set"))
    return;
  const auto roots = layerc::platform::profile_store_roots();
  if (!expect(layerc::platform::ensure_profile_store_roots(roots),
              "isolated profile store is available"))
    return;
  const auto scan = active_scan();

  // Session-started rules can arm timers without any direct binding.
  const auto timer_base = base_profile("timer-only");
  auto timer_profile = automation_profile(timer_base,
                                          AutomationEventKind::timer_fired);
  auto &timer_rules = timer_profile.automations.front().rules;
  timer_rules.front().source_filter = "pulse";
  AutomationRule start;
  start.id = "start";
  start.event = AutomationEventKind::session_started;
  start.actions = {{.kind = AutomationActionKind::create_timer,
                    .target = "pulse",
                    .name = {},
                    .property = {},
                    .value = literal(AutomationValue(
                        DurationValue{std::chrono::milliseconds{50}})),
                    .secondary = std::nullopt}};
  timer_rules.push_back(start);
  const StoredProfile timer_source{roots.config_profiles / "timer.json",
                                   timer_base};
  if (!expect(layerc::platform::write_profile_bundle(
                  timer_source.path, timer_base, std::nullopt, timer_profile).ok(),
              "timer-only sidecar is stored"))
    return;
  layerc::daemon::SessionRegistry timer_registry;
  timer_registry.configure(std::span{&timer_source, 1});
  timer_registry.refresh(scan);
  const auto timer_request = timer_registry.session_requests();
  const auto timer_demand =
      layerc::runtime::calculate_capability_demand(timer_request, false);
  if (!expect(timer_registry.session_count() == 1 &&
                  timer_registry.automation_count() == 1 &&
                  timer_request.size() == 1 && timer_request.front().automation &&
                  !timer_request.front().input &&
                  !timer_request.front().overlay &&
                  timer_demand.consumers(
                      layerc::runtime::Capability::automation) == 1 &&
                  timer_demand.consumers(
                      layerc::runtime::Capability::input) == 0 &&
                  timer_demand.consumers(
                      layerc::runtime::Capability::overlay) == 0,
              "timer-only profile demands automation without XI2 or overlay"))
    return;
  layerc::daemon::InputService timer_input{timer_registry};
  expect(timer_input.unavailable().empty(),
         "timer-only session has no detached XI2 fault");
  expect(timer_input.resources().handles == 0 &&
             timer_input.resources().objects == 0,
         "timer-only session owns no XI2 input resources");
  const auto timer_now = MonotonicClock::now();
  expect(timer_registry.next_automation_deadline(timer_now).has_value(),
         "session-started timer publishes a monotonic deadline");
  (void)timer_registry.poll_automation(timer_now + std::chrono::milliseconds{55});
  expect(timer_registry.sessions().front()
                 .automation.runtime()->snapshot().variables.at("count") ==
             AutomationValue(std::int64_t{1}),
         "timer-only rule runs without an input binding");

  // XI2 remains a passive source; the same session consumes an automation
  // input rule through feed_input even when its direct matcher is empty.
  const auto input_base = base_profile("input-only");
  const auto input_profile =
      automation_profile(input_base, AutomationEventKind::input_pressed);
  const StoredProfile input_source{roots.config_profiles / "input.json",
                                   input_base};
  if (!expect(layerc::platform::write_profile_bundle(
                  input_source.path, input_base, std::nullopt, input_profile).ok(),
              "input-rule sidecar is stored"))
    return;
  layerc::daemon::SessionRegistry input_registry;
  input_registry.configure(std::span{&input_source, 1});
  input_registry.refresh(scan);
  if (!expect(input_registry.session_count() == 1 &&
                  input_registry.automation_count() == 1 &&
                  input_registry.sessions().front().automation.profile().bindings.empty(),
              "input-rule automation is admitted without direct bindings"))
    return;
  const auto input_requests = input_registry.session_requests();
  const auto input_demand =
      layerc::runtime::calculate_capability_demand(input_requests, false);
  expect(input_requests.size() == 1 && input_requests.front().input &&
             !input_requests.front().overlay &&
             input_demand.consumers(layerc::runtime::Capability::input) == 1 &&
             input_demand.consumers(layerc::runtime::Capability::overlay) == 0,
         "automation input rule alone demands passive XI2");
  const auto input_now = MonotonicClock::now();
  const InputEvent press{{InputDevice::keyboard, 18, 0}, true};
  (void)input_registry.sessions().front().feed(press, input_now);
  expect(input_registry.sessions().front()
                 .automation.runtime()->snapshot().variables.at("count") ==
             AutomationValue(std::int64_t{1}),
         "automation input rule consumes the passive event");

  // A persistent write failure is one current fault, even after retries.
  const auto state_path = layerc::platform::automation_state_path(input_source.path);
  std::error_code error;
  std::filesystem::create_directories(state_path, error);
  if (!expect(!error, "state destination can be faulted in the fixture")) return;
  for (int attempt = 1; attempt <= 10; ++attempt) {
    (void)input_registry.poll_automation(
        input_now + std::chrono::milliseconds{1'100 * attempt});
    expect(input_registry.automation_diagnostics().size() == 1 &&
               input_registry.automation_persistence().find(input_source.path)->checkpoint_error.has_value(),
           "repeated checkpoint failures retain one reported fault");
  }
  expect(automation_status_response(input_registry).find(
              "\"availability\":\"degraded\",\"capability\":\"automation\","
              "\"diagnostics\":[") != std::string::npos &&
              automation_status_response(input_registry).find("\"reasons\":[\"profile_invalid\"]") != std::string::npos,
          "checkpoint failure degrades automation in the status response");
  expect(automation_status_response(input_registry).find(
              "\"diagnostics\":[\"automation checkpoint failed") !=
             std::string::npos,
         "status response includes the current checkpoint fault detail");
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "checkpoint fault is removed")) return;
  (void)input_registry.poll_automation(
      input_now + std::chrono::milliseconds{12'000});
  expect(input_registry.automation_diagnostics().empty() &&
             !input_registry.automation_persistence().find(input_source.path)->checkpoint_error.has_value(),
         "successful checkpoint clears the current fault");
  expect(automation_status_response(input_registry).find(
              "\"availability\":\"active\",\"capability\":\"automation\","
              "\"reasons\":[]") != std::string::npos,
          "successful checkpoint restores healthy automation status");
  expect(automation_status_response(input_registry).find(
              "\"diagnostics\"") == std::string::npos,
         "successful checkpoint removes the fault detail from status");
  const auto persisted =
      layerc::platform::load_automation_state(input_source.path, input_profile);
  expect(!persisted.error &&
             persisted.values.at("count") == AutomationValue(std::int64_t{1}),
         "recovered checkpoint writes the persistent value");

  // A failed final checkpoint survives the matched session and keeps the
  // automation timer/service alive until the durable write succeeds.
  (void)input_registry.sessions().front().feed(
      press, input_now + std::chrono::milliseconds{12'001});
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "stored state can be removed for the final fault"))
    return;
  std::filesystem::create_directories(state_path, error);
  if (!expect(!error, "final checkpoint destination can be faulted")) return;
  const auto retiring_now = input_now + std::chrono::milliseconds{12'002};
  input_registry.stop_sessions(retiring_now);
  const auto pending_requests = input_registry.session_requests();
  const auto pending_demand =
      layerc::runtime::calculate_capability_demand(pending_requests, false);
  layerc::daemon::AutomationService pending_service{input_registry};
  expect(input_registry.session_count() == 0 &&
             input_registry.pending_checkpoint_count() == 1 &&
             pending_demand.consumers(layerc::runtime::Capability::automation) ==
                 1 &&
             pending_demand.consumers(layerc::runtime::Capability::input) == 0 &&
             input_registry.next_automation_deadline(retiring_now)
                 .has_value() &&
             input_registry.automation_diagnostics().size() == 1 &&
             pending_service.resources().objects == 1 &&
             pending_service.resources().timers == 1,
         "retired write keeps one retry, demand, deadline, and visible fault");
  expect(automation_status_response(input_registry).find(
             "\"availability\":\"degraded\",\"capability\":\"automation\","
             "\"diagnostics\":[") != std::string::npos &&
             automation_status_response(input_registry).find("\"reasons\":[\"profile_invalid\"]") != std::string::npos,
         "retired checkpoint fault remains visible in status without sessions");
  expect(!input_registry.poll_automation(
             retiring_now + std::chrono::milliseconds{1'100}) &&
             input_registry.pending_checkpoint_count() == 1 &&
             input_registry.automation_diagnostics().size() == 1,
         "idle failed retry remains bounded to one pending source");

  input_registry.configure(std::span{&input_source, 1});
  input_registry.refresh(scan);
  const auto replaced =
      input_registry.sessions().front().automation.runtime()->snapshot();
  expect(input_registry.session_count() == 1 &&
             input_registry.automation_count() == 1 &&
             input_registry.pending_checkpoint_count() == 0 &&
             input_registry.automation_persistence().pending_count() == 1 &&
             replaced.variables.at("count") == AutomationValue(std::int64_t{2}),
         "a same-path replacement continues from the unsaved values");
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "retired checkpoint fault is removed")) return;
  const auto retry_now = retiring_now + std::chrono::milliseconds{2'200};
  expect(!input_registry.poll_automation(retry_now) &&
             input_registry.automation_persistence().pending_count() == 0 &&
             input_registry.automation_diagnostics().empty(),
         "the retried write succeeds under the replacement session");
  const auto retried =
      layerc::platform::load_automation_state(input_source.path, input_profile);
  expect(!retried.error &&
             retried.values.at("count") == AutomationValue(std::int64_t{2}),
         "the retried write stores the predecessor's unsaved value");

  // A state-load fault is current health and clears after a good checkpoint.
  if (!expect(layerc::test::write_file(state_path, "{invalid"),
              "corrupt state is isolated"))
    return;
  input_registry.configure(std::span{&input_source, 1});
  input_registry.refresh(scan);
  expect(input_registry.automation_diagnostics().size() == 1 &&
             input_registry.automation_persistence().find(input_source.path)->state_load_error.has_value(),
         "matched corrupt state reports one load fault");
  expect(!input_registry.poll_automation(
             retry_now + std::chrono::milliseconds{1'100}) &&
             input_registry.automation_diagnostics().empty(),
         "successful checkpoint repairs state and clears its load fault");

  // Removing the last matched consumer leaves only the failed write's demand;
  // completing that retry returns the automation service to idle.
  const auto final_now = retry_now + std::chrono::milliseconds{1'200};
  (void)input_registry.sessions().front().feed(press, final_now);
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "state can be removed for the final idle retry"))
    return;
  std::filesystem::create_directories(state_path, error);
  if (!expect(!error, "idle retry fault is isolated")) return;
  input_registry.stop_sessions(final_now + std::chrono::milliseconds{1});
  input_registry.configure({});
  input_registry.refresh({});
  expect(input_registry.pending_checkpoint_count() == 1 &&
             layerc::runtime::calculate_capability_demand(
                 input_registry.session_requests(), false)
                     .consumers(layerc::runtime::Capability::automation) == 1,
         "removed profile retains automation demand for the final write");
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "idle retry fault is removed")) return;
  expect(input_registry.poll_automation(
             final_now + std::chrono::milliseconds{1'100}) &&
             input_registry.pending_checkpoint_count() == 0 &&
             input_registry.automation_diagnostics().empty() &&
             layerc::runtime::calculate_capability_demand(
                 input_registry.session_requests(), false)
                     .consumers(layerc::runtime::Capability::automation) == 0 &&
             pending_service.resources().objects == 0 &&
             pending_service.resources().timers == 0,
         "successful idle retry clears fault and final automation demand");

  // An invalid sidecar still participates in demand and visible diagnostics.
  const auto invalid_base = base_profile("invalid-only");
  const StoredProfile invalid_source{roots.config_profiles / "invalid.json",
                                     invalid_base};
  if (!expect(layerc::test::write_file(
                  layerc::platform::automation_sidecar_path(invalid_source.path),
                  "{invalid"),
              "malformed automation sidecar is isolated"))
    return;
  layerc::daemon::SessionRegistry invalid_registry;
  invalid_registry.configure(std::span{&invalid_source, 1});
  invalid_registry.refresh({});
  expect(invalid_registry.automation_diagnostics().empty(),
         "unmatched invalid sidecar does not degrade automation");
  invalid_registry.refresh(scan);
  const auto invalid_requests = invalid_registry.session_requests();
  const auto invalid_demand =
      layerc::runtime::calculate_capability_demand(invalid_requests, false);
  expect(invalid_registry.session_count() == 1 &&
             invalid_registry.automation_count() == 0 &&
             invalid_requests.size() == 1 && invalid_requests.front().automation &&
             !invalid_requests.front().input &&
             !invalid_requests.front().overlay &&
             invalid_demand.consumers(layerc::runtime::Capability::automation) == 1 &&
             invalid_demand.consumers(layerc::runtime::Capability::input) == 0 &&
             !invalid_registry.automation_diagnostics().empty(),
         "malformed sidecar has automation demand and a load error without XI2");
  expect(automation_status_response(invalid_registry).find(
             "\"availability\":\"degraded\",\"capability\":\"automation\","
             "\"diagnostics\":[") != std::string::npos &&
             automation_status_response(invalid_registry).find("\"reasons\":[\"profile_invalid\"]") != std::string::npos,
         "malformed sidecar degrades automation in the status response");

  // Runtime faults stay keyed by automation while a healthy rule continues.
  const auto fault_base = base_profile("fault-isolation");
  auto fault_profile =
      automation_profile(fault_base, AutomationEventKind::session_started);
  fault_profile.variables.push_back(
      {.id = "fault_value",
       .type = AutomationValueType::number,
       .default_value = AutomationValue(0.0)});
  auto &good = fault_profile.automations.front();
  good.id = "good";
  good.rules.push_back(good.rules.front());
  good.rules.back().id = "input";
  good.rules.back().event = AutomationEventKind::input_pressed;
  good.rules.back().source_filter = "keyboard:18";
  AutomationRule broken;
  broken.id = "broken";
  broken.priority = 20;
  broken.event = AutomationEventKind::session_started;
  AutomationExpression divide;
  divide.kind = ExpressionKind::binary;
  divide.operation = "divide";
  divide.arguments = {literal(AutomationValue(1.0)),
                      literal(AutomationValue(0.0))};
  broken.actions = {{.kind = AutomationActionKind::set_variable,
                     .target = "fault_value",
                     .name = {},
                     .property = {},
                     .value = divide,
                     .secondary = std::nullopt}};
  fault_profile.automations.push_back(
      {.id = "bad", .name = "Bad", .rules = {broken}});
  const StoredProfile fault_source{roots.config_profiles / "fault.json",
                                   fault_base};
  if (!expect(layerc::platform::write_profile_bundle(
                  fault_source.path, fault_base, std::nullopt, fault_profile).ok(),
              "fault-isolation sidecar is stored"))
    return;
  layerc::daemon::SessionRegistry fault_registry;
  fault_registry.configure(std::span{&fault_source, 1});
  fault_registry.refresh(scan);
  if (!expect(fault_registry.session_count() == 1 &&
                  fault_registry.automation_count() == 1,
              "fault-isolation session is admitted"))
    return;
  expect(fault_registry.sessions().front().automation.health().runtime_faults.size() ==
                 1 &&
             fault_registry.automation_diagnostics().size() == 1 &&
             fault_registry.sessions().front()
                     .automation.runtime()->snapshot().variables.at("count") ==
                 AutomationValue(std::int64_t{1}),
         "one runtime fault is reported while the healthy automation commits");
  (void)fault_registry.sessions().front().feed(press, MonotonicClock::now());
  expect(fault_registry.automation_diagnostics().size() == 1 &&
             fault_registry.sessions().front()
                     .automation.runtime()->snapshot().variables.at("count") ==
                 AutomationValue(std::int64_t{2}),
         "runtime fault remains bounded and unrelated automation continues");
  expect(automation_status_response(fault_registry).find(
              "\"availability\":\"degraded\",\"capability\":\"automation\","
              "\"diagnostics\":[") != std::string::npos &&
              automation_status_response(fault_registry).find("\"reasons\":[\"profile_invalid\"]") != std::string::npos,
          "runtime fault degrades automation in the status response");
  expect(automation_status_response(fault_registry).find(
              "automation fault: bad:") != std::string::npos,
         "status response names the faulted automation");

  // A new configuration drops identities from the old automation runtime.
  fault_profile.automations.pop_back();
  if (!expect(layerc::platform::write_profile_bundle(
                  fault_source.path, fault_base, std::nullopt, fault_profile).ok(),
              "faulted automation is removed from the sidecar"))
    return;
  fault_registry.configure(std::span{&fault_source, 1});
  fault_registry.refresh(scan);
  expect(fault_registry.session_count() == 1 &&
             fault_registry.sessions().front().automation.runtime() &&
             fault_registry.sessions().front()
                 .automation.health().runtime_faults.empty() &&
             fault_registry.automation_diagnostics().empty() &&
             automation_status_response(fault_registry).find(
                 "\"availability\":\"active\",\"capability\":\"automation\","
                 "\"reasons\":[]") != std::string::npos,
         "replacing a faulted sidecar removes its stale runtime identity");
}

void test_registry_fresh_admission_state() {
  using layerc::test::expect;
  layerc::test::TempDirectory temp{"layerc-registry-readmission"};
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME", temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  if (!expect(config.applied() && data.applied(), "readmission roots are isolated"))
    return;
  const auto roots = layerc::platform::profile_store_roots();
  if (!expect(layerc::platform::ensure_profile_store_roots(roots),
              "readmission store is available"))
    return;
  const auto base = base_profile("readmission");
  auto automation = automation_profile(base, AutomationEventKind::input_pressed);
  auto stopping = automation.automations.front().rules.front();
  stopping.id = "stopping";
  stopping.event = AutomationEventKind::session_stopping;
  stopping.source_filter.clear();
  stopping.actions.front().value = literal(AutomationValue(std::int64_t{10}));
  automation.automations.front().rules.push_back(std::move(stopping));
  const StoredProfile source{roots.config_profiles / "readmission.json", base};
  if (!expect(layerc::platform::write_profile_bundle(
                  source.path, base, std::nullopt, automation).ok(),
              "readmission profile and stopping rule are stored"))
    return;
  layerc::daemon::SessionRegistry registry;
  registry.configure(std::span{&source, 1});
  const auto scan = active_scan();
  registry.refresh(scan);
  if (!expect(registry.automation_count() == 1, "first runtime is admitted"))
    return;
  const InputEvent press{{InputDevice::keyboard, 18, 0}, true};
  auto count = [&registry] {
    return registry.sessions().front().automation.runtime()->snapshot()
        .variables.at("count");
  };
  (void)registry.sessions().front().feed(press, MonotonicClock::now());
  registry.checkpoint_automation(MonotonicClock::now());
  registry.refresh({});
  const auto stopped = layerc::platform::load_automation_state(source.path, automation);
  expect(registry.session_count() == 0 && registry.pending_checkpoint_count() == 0 &&
             !stopped.error && stopped.values.at("count") == AutomationValue(std::int64_t{11}),
         "successful retirement persists both input and session-stopping changes");
  registry.refresh(scan); // No configure(): its old state snapshot must not win.
  if (!expect(registry.automation_count() == 1, "unmatched profile is readmitted"))
    return;
  expect(count() == AutomationValue(std::int64_t{11}),
         "fresh admission reloads latest durable values without configure");

  // An older failed final write stays with the owner across unmatched
  // intervals: a readmission continues from its unsaved values, and a retry
  // that completes while unmatched updates the next runtime.
  (void)registry.sessions().front().feed(press, MonotonicClock::now());
  const auto state_path = layerc::platform::automation_state_path(source.path);
  std::error_code error;
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "readmission state can be faulted")) return;
  std::filesystem::create_directories(state_path, error);
  if (!expect(!error, "readmission checkpoint fault is installed")) return;
  registry.refresh({});
  expect(registry.pending_checkpoint_count() == 1,
         "failed retirement retains one pending checkpoint");
  registry.refresh(scan);
  expect(registry.session_count() == 1 && registry.automation_count() == 1 &&
             count() == AutomationValue(std::int64_t{22}) &&
             registry.automation_persistence().pending_count() == 1,
         "a readmission continues from the pending predecessor's values");
  registry.refresh({});
  std::filesystem::remove(state_path, error);
  if (!expect(!error, "readmission checkpoint fault is removed")) return;
  const auto retry_now = MonotonicClock::now() + std::chrono::milliseconds{1'100};
  expect(registry.poll_automation(retry_now) && registry.session_count() == 0 &&
             registry.pending_checkpoint_count() == 0,
         "injected retry deadline completes the checkpoint while unmatched");
  registry.refresh(scan);
  if (!expect(registry.automation_count() == 1, "runtime starts after unmatched retry"))
    return;
  expect(count() == AutomationValue(std::int64_t{32}),
         "admission after unmatched retry loads the successful stopping checkpoint");

  // Explicit reloads still apply external state to retained clean runtimes,
  // but never overwrite a retained runtime's uncheckpointed changes.
  auto *retained = registry.sessions().front().automation.runtime();
  if (!expect(layerc::platform::write_automation_state(
                  source.path, automation, {{"count", AutomationValue(std::int64_t{31})}}).ok(),
              "external state for explicit reload is stored"))
    return;
  registry.configure(std::span{&source, 1});
  registry.refresh(scan);
  expect(registry.sessions().front().automation.runtime() == retained &&
             count() == AutomationValue(std::int64_t{31}),
         "explicit configuration reload retains and updates a clean runtime");
  (void)registry.sessions().front().feed(press, MonotonicClock::now());
  if (!expect(layerc::platform::write_automation_state(
                  source.path, automation, {{"count", AutomationValue(std::int64_t{44})}}).ok(),
              "competing external state is isolated"))
    return;
  registry.configure(std::span{&source, 1});
  registry.refresh(scan);
  expect(registry.sessions().front().automation.runtime() == retained &&
             count() == AutomationValue(std::int64_t{32}),
         "explicit reload preserves a dirty retained runtime's values");
  registry.stop_sessions(MonotonicClock::now());
}

void test_status_diagnostic_bound() {
  using layerc::test::expect;
  std::vector<std::string> faults(1024, std::string(1024, 'x'));
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::automation, 1);
  layerc::daemon::LinuxStatusSnapshot status;
  status.capabilities.push_back(
      {layerc::runtime::Capability::automation,
       layerc::daemon::CapabilityAvailability::inactive, {}, {}});
  layerc::daemon::derive_capability_status(
      status, demand,
      {.input = {},
       .overlay = {},
       .automation = std::span<const std::string>{faults},
       .visual = {}});
  const auto &details = status.capabilities.front().diagnostics;
  expect(details.size() == 17 && details.front().size() == 512 &&
             details.back() == "additional diagnostics omitted",
         "capability status bounds fault count and detail bytes");
  const layerc::runtime::DaemonKernel kernel;
  const auto response = layerc::daemon::handle_control_request(
      layerc::platform::encode_control_request(
          layerc::platform::ControlCommand::status),
      kernel, status);
  expect(response.response.size() < layerc::platform::control_frame_limit &&
             response.response.find("additional diagnostics omitted") !=
                 std::string::npos,
         "bounded fault detail keeps the status control frame sendable");
}

void test_registry_trace_lifetime() {
  using layerc::test::expect;
  layerc::test::TempDirectory temp{"layerc-trace-registry"};
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME", temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  if (!expect(config.applied() && data.applied(), "trace tests own disposable XDG roots")) return;
  const auto roots = layerc::platform::profile_store_roots();
  if (!expect(layerc::platform::ensure_profile_store_roots(roots), "private trace store exists")) return;
  const auto base = base_profile("trace-only");
  auto automation = automation_profile(base, AutomationEventKind::input_pressed);
  const StoredProfile source{roots.config_profiles / "trace.json", base};
  if (!expect(layerc::platform::write_profile_bundle(
          source.path, base, std::nullopt, automation).ok(), "trace-only bundle is stored")) return;
  layerc::daemon::SessionRegistry registry;
  layerc::daemon::TraceLeaseHost host;
  registry.configure(std::span{&source, 1});
  const auto scan = active_scan();
  registry.refresh(scan);
  auto *runtime = registry.sessions().front().automation.runtime();
  if (!expect(runtime && !runtime->trace_enabled() && runtime->trace().capacity() == 0,
              "ordinary automation allocates no trace buffer")) return;
  const auto now = MonotonicClock::now();
  TraceControlRequest request{TraceControlOperation::subscribe, base.id, "client-a", 0, 0};
  const auto failure_code = [](const auto &result)
      -> std::optional<layerc::core::TraceControlError> {
    if (const auto *failure = std::get_if<layerc::core::TraceControlFailure>(&result))
      return failure->code;
    return std::nullopt;
  };
  auto exchange = [&](const TraceControlRequest &value) -> std::optional<TraceControlReply> {
    const auto response = host.handle(value, registry.sessions(), now);
    if (!expect(std::holds_alternative<TraceControlReply>(response), "valid trace request succeeds"))
      return std::nullopt;
    return std::get<TraceControlReply>(response);
  };
  auto reply = exchange(request);
  if (!reply) return;
  expect(reply->reset && reply->generation != 0 && runtime->trace_enabled() &&
             runtime->snapshot().variables.at("count") == AutomationValue(std::int64_t{0}),
         "subscription only enables observation; it never evaluates a root");
  expect(host.next_deadline() == std::optional<TimePoint>{now + trace_control_lease},
         "the earliest lease expiry is the engine deadline");
  auto later = request;
  later.client_id = "client-deadline";
  const auto later_reply = host.handle(later, registry.sessions(), now + Milliseconds{1});
  expect(std::holds_alternative<TraceControlReply>(later_reply) &&
             host.next_deadline() == std::optional<TimePoint>{now + trace_control_lease},
         "a later subscriber cannot extend the earliest deadline");
  auto drop_later = later;
  drop_later.operation = TraceControlOperation::unsubscribe;
  (void)host.handle(drop_later, registry.sessions(), now + Milliseconds{1});
  request.operation = TraceControlOperation::poll;
  request.generation = reply->generation; request.cursor = reply->cursor;
  AutomationEvent root;
  root.kind = AutomationEventKind::input_pressed;
  root.source_id = "fixture"; root.timestamp = now; root.origin_token = 1;
  (void)runtime->process(root);
  reply = exchange(request);
  if (!reply) return;
  const auto imported = parse_layerc_trace(reply->packet);
  expect(std::holds_alternative<std::vector<AutomationEvent>>(imported) &&
             std::get<std::vector<AutomationEvent>>(imported).size() == 1 &&
             reply->cursor > request.cursor,
         "bounded packets carry a replayable root and advance the sequence cursor");
  const auto retry = host.handle(request, registry.sessions(), now);
  const auto *again = std::get_if<TraceControlReply>(&retry);
  expect(again != nullptr && again->cursor == reply->cursor && again->packet == reply->packet,
         "a lost reply is fetched again with the unacknowledged cursor");
  const auto stale = request.cursor;
  request.cursor = reply->cursor;
  if (!exchange(request)) return; // acknowledges the delivered batch
  auto replay = request; replay.cursor = stale;
  expect(failure_code(host.handle(replay, registry.sessions(), now)) ==
             layerc::core::TraceControlError::invalid_cursor,
         "a stale cursor cannot replay an acknowledged packet");
  auto wrong = request; wrong.profile_id = "unrelated";
  expect(failure_code(host.handle(wrong, registry.sessions(), now)) ==
             layerc::core::TraceControlError::client_conflict && runtime->trace_enabled(),
         "one client cannot affect another profile subscription");
  const auto a = request;
  auto b = request; b.client_id = "client-b"; b.operation = TraceControlOperation::subscribe;
  if (!exchange(b)) return;
  auto unsubscribe = request; unsubscribe.operation = TraceControlOperation::unsubscribe;
  if (!exchange(unsubscribe)) return;
  expect(runtime->trace_enabled(), "another subscriber keeps trace demand alive");
  unsubscribe.client_id = b.client_id;
  if (!exchange(unsubscribe)) return;
  expect(!runtime->trace_enabled() && runtime->trace().empty() && runtime->trace().capacity() == 0,
         "last unsubscribe releases both capture demand and trace capacity");

  request = a; request.operation = TraceControlOperation::subscribe;
  reply = exchange(request);
  if (!reply) return;
  request.operation = TraceControlOperation::poll;
  request.generation = reply->generation; request.cursor = reply->cursor;
  (void)runtime->process(root);
  reply = exchange(request);
  if (!reply) return;
  request.cursor = reply->cursor;
  for (std::size_t i = 0; i < max_automation_trace_entries + 8; ++i) {
    root.origin_token = i + 10;
    (void)runtime->process(root);
  }
  reply = exchange(request);
  if (!reply) return;
  expect(reply->gap && reply->packet.size() <= trace_control_packet_bytes &&
             runtime->trace().size() <= max_automation_trace_entries,
         "a slow subscriber sees explicit truncation without unbounded growth");

  const auto old_generation = request.generation;
  automation.automations.front().rules.front().actions.front().value =
      literal(AutomationValue(std::int64_t{2}));
  if (!expect(layerc::platform::write_profile_bundle(
          source.path, base, std::nullopt, automation).ok(), "changed graph is durably stored")) return;
  registry.configure(std::span{&source, 1}); registry.refresh(scan);
  runtime = registry.sessions().front().automation.runtime();
  host.reconcile(registry.sessions());
  expect(runtime->trace_enabled(),
         "a rebuilt session inherits the live subscriber's trace demand");
  reply = exchange(request);
  if (!reply) return;
  expect(reply->reset && reply->generation != old_generation && reply->cursor == 0 &&
             runtime->trace_enabled(), "runtime replacement has an explicit new trace generation");
  request.generation = reply->generation; request.cursor = reply->cursor;
  reply = exchange(request);
  if (!reply) return;
  expect(!reply->reset && reply->packet.empty(), "empty new generation cannot reuse old roots");
  expect(host.expire(now + trace_control_lease + Milliseconds{1}, registry.sessions()),
         "the engine timer observes the lapsed lease");
  expect(!runtime->trace_enabled() && runtime->trace().capacity() == 0 &&
             failure_code(host.handle(request, registry.sessions(),
                                      now + trace_control_lease + Milliseconds{1})) ==
                 layerc::core::TraceControlError::expired,
         "abandoned clients expire on the daemon monotonic event-loop deadline");

  request.operation = TraceControlOperation::subscribe;
  for (std::size_t i = 0; i < trace_control_client_limit; ++i) {
    request.client_id = "limit-" + std::to_string(i);
    if (!exchange(request)) return;
  }
  request.client_id = "too-many";
  expect(failure_code(host.handle(request, registry.sessions(), now)) ==
             layerc::core::TraceControlError::subscriber_limit,
         "subscriber count is bounded independently of the control connection limit");
  expect(host.expire(now + trace_control_lease, registry.sessions()) &&
             !runtime->trace_enabled() && runtime->trace().capacity() == 0 &&
             !host.next_deadline() &&
             !host.expire(now + trace_control_lease, registry.sessions()),
         "at the exact deadline all disconnected subscribers release their trace buffer and deadline");
  expect(failure_code(host.handle(request, registry.sessions(), TimePoint::max())) ==
             layerc::core::TraceControlError::clock_limit,
         "lease clock overflow is refused instead of creating immortal demand");
  registry.stop_sessions(MonotonicClock::now());
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(test_registry_automation_only_and_health),
                 LAYERC_TEST_CASE(test_registry_fresh_admission_state),
                 LAYERC_TEST_CASE(test_status_diagnostic_bound),
                 LAYERC_TEST_CASE(test_registry_trace_lifetime))
