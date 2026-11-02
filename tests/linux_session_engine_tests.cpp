#include "input_service.hpp"
#include "session_engine.hpp"
#include "overlay_service.hpp"
#include "test_support.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace core = layerc::core;
namespace daemon = layerc::daemon;
namespace platform = layerc::platform;
namespace runtime = layerc::runtime;
using layerc::test::expect;

class FakeCapture final : public daemon::VisualCaptureSession {
public:
  bool start() override { return true; }
  void stop() noexcept override {}
  void set_quality(core::CaptureQuality) noexcept override {}
  void set_sample_rate(std::uint8_t) noexcept override {}
  layerc::capture::CapturePoll poll(core::TimePoint) override { return {}; }
  runtime::RuntimeResourceStats resources() const noexcept override { return {}; }
};

class FakeDisplay final : public daemon::DisplaySource,
                          public daemon::PassiveInputSource,
                          public daemon::OverlayBackend,
                          public daemon::CaptureFactory {
public:
  platform::WindowScan scan;
  unsigned shows{};
  unsigned captures{};
  bool opened{};
  bool shown{};
  bool anchor_active{};
  bool fail_show{};
  std::optional<std::uint64_t> xid;
  bool refuse_capture{};
  std::string failed_panel;
  std::map<std::string, std::string, std::less<>> visible;

  bool has_windows() const override { return true; }
  platform::WindowScan windows() const override { return scan; }
  bool has_monitors() const override { return true; }
  platform::MonitorScan monitors() const override { return {}; }
  daemon::OverlayBackend *overlay() override { return this; }
  daemon::CaptureFactory *capture() override { return this; }
  daemon::PassiveInputSource *input() override { return this; }
  bool input_available() const noexcept override { return true; }
  std::string_view input_name() const noexcept override { return "XI2"; }
  bool open_input(std::string &) override { opened = true; return true; }
  void close_input() noexcept override { opened = false; }
  bool input_open() const noexcept override { return opened; }
  std::vector<std::string> input_unavailable(daemon::InputDeviceNeeds) const override { return {}; }
  runtime::RuntimeResourceStats input_resources() const noexcept override { return {}; }
  bool show(std::string_view key, const platform::OverlayFrame &frame) override {
    ++shows;
    const std::string owned_key(key);
    if (fail_show &&
        (failed_panel.empty() || frame.panel.id == failed_panel)) {
      visible.erase(owned_key);
      shown = false;
      return false;
    }
    visible[owned_key] = frame.panel.id;
    shown = true;
    anchor_active = frame.anchor_active;
    return true;
  }
  void hide(std::string_view key) noexcept override {
    visible.erase(std::string(key));
    shown = !visible.empty();
  }
  void retain(std::span<const std::string> keys) noexcept override {
    std::erase_if(visible, [&](const auto &entry) {
      return std::ranges::find(keys, entry.first) == keys.end();
    });
    shown = !visible.empty();
  }
  std::vector<std::string> unavailable() const override { return {}; }
  std::optional<std::uint64_t> x11_window(std::uint64_t) const override {
    return xid;
  }
  std::unique_ptr<daemon::VisualCaptureSession> create_capture(
      std::string_view, core::WindowId, core::CaptureQuality, std::uint8_t) override {
    ++captures;
    return refuse_capture ? nullptr : std::make_unique<FakeCapture>();
  }
};

void neutral_session_engine() {
  layerc::test::TempDirectory temp("layerc-neutral-engine");
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  const auto roots = platform::profile_store_roots();
  if (!expect(platform::ensure_profile_store_roots(roots), "private engine fixture roots exist")) return;
  core::GameProfile profile;
  profile.id = "engine";
  profile.name = "Engine";
  profile.game.executable_name = "fixture.exe";
  profile.cooldowns.push_back({.id = "dash", .name = "Dash",
                                .duration = std::chrono::seconds(5)});
  profile.bindings.push_back({.cooldown_id = "dash",
      .chord = {{{core::InputDevice::keyboard, 18, 0}}}});
  profile.panels.push_back({.id = "main", .name = "Main", .bar = {},
                            .cooldown_ids = {"dash"}});
  core::VisualProfile visual;
  visual.profile_id = profile.id;
  core::PixelRule pixel;
  pixel.id = "ready";
  pixel.name = "Ready";
  visual.observations.push_back(pixel);
  visual.triggers.push_back({.cooldown_id = "dash", .observation_id = "ready",
                             .keep_input = true});
  core::AutomationProfile automation;
  automation.profile_id = profile.id;
  core::AutomationRule rule;
  rule.id = "start";
  rule.event = core::AutomationEventKind::session_started;
  core::AutomationAction action;
  action.kind = core::AutomationActionKind::trigger_cooldown;
  action.target = "dash";
  rule.actions.push_back(action);
  // The input event, not session admission, must trigger the tested cooldown.
  automation.automations.push_back({.id = "automation", .name = "Automation",
                                    .rules = {rule}});
  automation.automations.front().rules.front().event = core::AutomationEventKind::timer_fired;
  const platform::StoredProfile stored{roots.config_profiles / "engine.json", profile};
  if (!expect(platform::write_profile_bundle(stored.path, profile, visual, automation).ok(),
              "the engine fixture and enabled sidecars are stored")) return;
  FakeDisplay provider;
  platform::WindowSnapshot window;
  window.window = 9;
  window.executable_name = "fixture.exe";
  window.width = 800;
  window.height = 600;
  window.active = true;
  provider.scan.windows.push_back(window);
  provider.scan.active = 9;
  daemon::DisplayRoute route{.windows = &provider, .monitors = &provider,
      .overlay = &provider,
      .capture = {.native = &provider, .native_windows = true},
      .native_overlay = true,
      .inputs = {&provider}};
  daemon::SessionEngine engine;
  runtime::DaemonKernel kernel(engine.runtime_services());
  daemon::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  engine.refresh(std::span{&stored, 1}, true, route, kernel, status, diagnostics);
  expect(engine.session_count() == 1, "a native provider admits a matching session without X11");
  for (const auto capability : {runtime::Capability::input, runtime::Capability::automation,
                                runtime::Capability::overlay, runtime::Capability::visual})
    expect(kernel.demand().required(capability), "every enabled module contributes capability demand");
  expect(provider.opened && provider.captures == 1,
         "neutral input and capture services activate their supplied providers");
  const core::InputEvent press{{core::InputDevice::keyboard, 18, 0}, true};
  daemon::DisplayDispatch dispatched;
  engine.feed_input(std::span{&press, 1}, dispatched);
  expect(dispatched.triggered == std::vector<std::string>{"dash"},
         "provider input reaches the shared cooldown session");
  (void)engine.render(core::MonotonicClock::now());
  expect(provider.shows != 0 && provider.shown && provider.anchor_active &&
             engine.session_diagnostics().front().overlay_windows == 1,
         "the neutral overlay paints a shown panel with active-anchor state");
  kernel.quiesce();
  (void)engine.render(core::MonotonicClock::now());
  expect(!provider.shown && !provider.opened,
         "quiescence releases provider resources and rendering cannot recreate overlays");
  engine.stop_sessions(core::MonotonicClock::now());
}

[[nodiscard]] core::AutomationProfile provider_loss_automation(
    std::string profile_id) {
  using core::AutomationActionKind;
  using core::AutomationEventKind;
  using core::AutomationValue;
  using core::AutomationValueType;

  core::AutomationProfile automation;
  automation.profile_id = std::move(profile_id);
  automation.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0}),
       .persistent = true});
  const auto one = core::expr::literal(AutomationValue(std::int64_t{1}));
  const auto ten = core::expr::literal(AutomationValue(std::int64_t{10}));
  core::AutomationAction increment_one{
      .kind = AutomationActionKind::increment_variable,
      .target = "count",
      .name = {},
      .property = {},
      .value = one,
      .secondary = std::nullopt};
  core::AutomationAction increment_ten{
      .kind = AutomationActionKind::increment_variable,
      .target = "count",
      .name = {},
      .property = {},
      .value = ten,
      .secondary = std::nullopt};
  core::AutomationAction create_timer{
      .kind = AutomationActionKind::create_timer,
      .target = "pulse",
      .name = {},
      .property = {},
      .value = core::expr::literal(AutomationValue(
          core::DurationValue{core::Milliseconds{2000}})),
      .secondary = std::nullopt,
      .repeating = true};
  core::AutomationRule start;
  start.id = "start";
  start.event = AutomationEventKind::session_started;
  start.actions = {increment_one, create_timer};
  core::AutomationRule timer;
  timer.id = "timer_fired";
  timer.event = AutomationEventKind::timer_fired;
  timer.actions = {increment_one};
  core::AutomationRule stopping;
  stopping.id = "session_stopping";
  stopping.event = AutomationEventKind::session_stopping;
  stopping.actions = {increment_ten};
  automation.automations.push_back(
      {.id = "lifecycle", .name = "Lifecycle", .rules = {start, timer, stopping}});
  return automation;
}

[[nodiscard]] core::GameProfile provider_loss_profile() {
  core::GameProfile profile;
  profile.id = "provider-loss";
  profile.name = "Provider loss";
  profile.game.executable_name = "fixture.exe";
  return profile;
}

void provider_loss_case(const std::string_view mode,
                        const bool checkpoint_fault) {
  layerc::test::TempDirectory temp("layerc-provider-loss");
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  if (!expect(config.applied() && data.applied(),
              "provider-loss roots are isolated"))
    return;
  const auto roots = platform::profile_store_roots();
  if (!expect(platform::ensure_profile_store_roots(roots),
              "provider-loss store roots exist"))
    return;

  auto profile = provider_loss_profile();
  const auto automation = provider_loss_automation(profile.id);
  const platform::StoredProfile stored{
      roots.config_profiles / "provider-loss.json", profile};
  if (!expect(platform::write_profile_bundle(stored.path, profile, std::nullopt,
                                             automation).ok(),
              "provider-loss profile and automation sidecar are stored"))
    return;

  FakeDisplay provider;
  platform::WindowSnapshot window;
  window.window = 9;
  window.executable_name = "fixture.exe";
  window.width = 800;
  window.height = 600;
  window.active = true;
  provider.scan.windows.push_back(window);
  provider.scan.active = 9;
  const daemon::DisplayRoute route{.windows = &provider,
                                   .monitors = &provider,
                                   .overlay = &provider,
                                   .capture = {},
                                   .inputs = {}};

  daemon::SessionEngine engine;
  runtime::DaemonKernel kernel(engine.runtime_services());
  daemon::LinuxStatusSnapshot status;
  status.capabilities.push_back(
      {.capability = runtime::Capability::automation,
       .availability = runtime::CapabilityAvailability::inactive,
       .reasons = {},
       .diagnostics = {}});
  std::vector<std::string> diagnostics;
  engine.refresh(std::span{&stored, 1}, true, route, kernel, status, diagnostics);
  if (!expect(engine.session_count() == 1 &&
                  kernel.demand().consumers(runtime::Capability::automation) == 1 &&
                  kernel.demand().consumers(runtime::Capability::input) == 0 &&
                  kernel.demand().consumers(runtime::Capability::overlay) == 0 &&
                  kernel.demand().consumers(runtime::Capability::visual) == 0,
              "the matching profile admits one automation-only session")) {
    kernel.quiesce();
    return;
  }
  const auto admitted_at = core::MonotonicClock::now();
  if (!expect(engine.next_deadline(admitted_at).has_value(),
              "session start arms its repeating timer deadline")) {
    kernel.quiesce();
    return;
  }

  const auto state_path = platform::automation_state_path(stored.path);
  std::error_code error;
  if (checkpoint_fault) {
    std::filesystem::create_directories(state_path, error);
    if (!expect(!error, "persistent state destination is faulted privately")) {
      kernel.quiesce();
      return;
    }
  }

  std::vector<platform::StoredProfile> current_profiles{stored};
  bool configuration_changed = false;
  if (mode == "remove") {
    current_profiles.clear();
    configuration_changed = true;
  } else if (mode == "disable") {
    current_profiles.front().profile.enabled = false;
    configuration_changed = true;
  } else if (mode != "keep") {
    expect(false, "provider-loss mode is recognized");
    kernel.quiesce();
    return;
  }
  diagnostics.clear();
  engine.refresh(current_profiles, configuration_changed, {}, kernel, status,
                 diagnostics);
  expect(engine.session_count() == 0 && engine.session_diagnostics().empty(),
         "provider loss retires sessions through the normal registry path");
  expect(kernel.demand().consumers(runtime::Capability::input) == 0 &&
             kernel.demand().consumers(runtime::Capability::overlay) == 0 &&
             kernel.demand().consumers(runtime::Capability::visual) == 0,
         "retirement leaves no input, overlay or visual demand");

  if (checkpoint_fault) {
    const auto automation_row = [&]() -> const runtime::CapabilityStatus * {
      const auto found = std::ranges::find(
          status.capabilities, runtime::Capability::automation,
          &runtime::CapabilityStatus::capability);
      return found == status.capabilities.end() ? nullptr : &*found;
    };
    const auto *row = automation_row();
    if (!expect(kernel.demand().consumers(runtime::Capability::automation) == 1 &&
                    row != nullptr &&
                    row->availability == runtime::CapabilityAvailability::degraded &&
                    std::ranges::any_of(row->diagnostics, [](const auto &line) {
                      return line.starts_with("automation checkpoint failed");
                    }),
                "failed final checkpoint retains automation demand and status")) {
      kernel.quiesce();
      return;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
      const auto due = engine.next_deadline(core::MonotonicClock::now());
      if (!expect(due.has_value(), "a failed checkpoint publishes its retry deadline")) {
        kernel.quiesce();
        return;
      }
      (void)engine.render(*due);
      engine.refresh_status(kernel, status);
      const auto retry = engine.next_deadline(*due);
      const auto *retry_row = automation_row();
      expect(engine.session_count() == 0 &&
                 kernel.demand().consumers(runtime::Capability::automation) == 1,
             "failed retries retain one session-less automation consumer");
      expect(retry.has_value() && *retry > *due &&
                 *retry <= *due + std::chrono::seconds{2},
             "each failed checkpoint has one bounded retry deadline");
      expect(retry_row != nullptr &&
                 retry_row->availability == runtime::CapabilityAvailability::degraded &&
                 std::ranges::any_of(retry_row->diagnostics, [](const auto &line) {
                   return line.starts_with("automation checkpoint failed");
                 }),
             "each failed retry remains visible in automation status");
    }
    std::filesystem::remove_all(state_path, error);
    if (!expect(!error, "the private checkpoint fault is removed")) {
      kernel.quiesce();
      return;
    }
    const auto due = engine.next_deadline(core::MonotonicClock::now());
    if (!expect(due.has_value(), "checkpoint recovery retains its retry deadline")) {
      kernel.quiesce();
      return;
    }
    expect(engine.render(*due),
           "a successful retired checkpoint changes automation demand");
    diagnostics.clear();
    engine.refresh(current_profiles, false, {}, kernel, status, diagnostics);
    engine.refresh_status(kernel, status);
    expect(kernel.demand().consumers(runtime::Capability::automation) == 0 &&
               engine.session_count() == 0 && !engine.next_deadline(*due) &&
               engine.session_diagnostics().empty(),
           "successful flush removes all retired automation state and demand");
    const auto flushed_automation_row = std::ranges::find(
        status.capabilities, runtime::Capability::automation,
        &runtime::CapabilityStatus::capability);
    expect(flushed_automation_row != status.capabilities.end() &&
               flushed_automation_row->diagnostics.empty(),
           "successful flush clears the checkpoint diagnostic");
  } else {
    expect(kernel.demand().consumers(runtime::Capability::automation) == 0 &&
               !engine.next_deadline(core::MonotonicClock::now()),
           "healthy retirement leaves no automation demand or deadline");
  }

  const auto after_retirement = core::MonotonicClock::now();
  for (const auto elapsed : {std::chrono::seconds{10},
                             std::chrono::seconds{12}}) {
    const auto poll = after_retirement + elapsed;
    (void)engine.render(poll);
    expect(engine.session_count() == 0 && !engine.next_deadline(poll),
           "a retired timer cannot recreate work at a later poll");
  }
  const auto state = platform::load_automation_state(stored.path, automation);
  if (!expect(!state.error && state.values.contains("count"),
              "the stopping checkpoint is readable")) {
    kernel.quiesce();
    return;
  }
  expect(state.values.at("count") == core::AutomationValue(std::int64_t{11}),
         "start and stopping rules persist exactly once without an old timer fire");

  const auto before_readmission = core::MonotonicClock::now();
  diagnostics.clear();
  engine.refresh(current_profiles, false, route, kernel, status, diagnostics);
  if (mode == "keep") {
    if (!expect(engine.session_count() == 1,
                "a retained profile is readmitted when its window returns")) {
      kernel.quiesce();
      return;
    }
    const auto after_readmission = core::MonotonicClock::now();
    const auto checkpoint_due = engine.next_deadline(after_readmission);
    if (!expect(checkpoint_due.has_value(),
                "fresh admission arms a persistence checkpoint")) {
      kernel.quiesce();
      return;
    }
    (void)engine.render(*checkpoint_due);
    const auto fresh_timer = engine.next_deadline(*checkpoint_due);
    expect(fresh_timer.has_value() && *fresh_timer > *checkpoint_due &&
               *fresh_timer <= *checkpoint_due + std::chrono::milliseconds{2200},
           "readmission arms a fresh repeating timer after the checkpoint flush");
    const auto resumed = platform::load_automation_state(stored.path, automation);
    expect(!resumed.error && resumed.values.contains("count") &&
               resumed.values.at("count") == core::AutomationValue(std::int64_t{12}),
           "fresh admission resumes from the last stopping checkpoint");
    engine.stop_sessions(*checkpoint_due);
    expect(engine.session_count() == 0 && !engine.next_deadline(*checkpoint_due),
           "the retained test session retires normally before teardown");
  } else {
    expect(engine.session_count() == 0 && !engine.next_deadline(before_readmission),
           "removed or disabled profiles stay absent when the window returns");
  }
  kernel.quiesce();
}

void provider_loss_lifecycle() {
  for (const auto mode : {std::string_view{"keep"}, std::string_view{"remove"},
                          std::string_view{"disable"}}) {
    provider_loss_case(mode, false);
    provider_loss_case(mode, true);
  }
}

[[nodiscard]] core::GameProfile overlay_retry_profile(std::string id) {
  core::GameProfile profile;
  profile.id = std::move(id);
  profile.name = "Overlay retry";
  profile.game.executable_name = "fixture.exe";
  profile.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds{5}});
  core::Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.style = core::PanelStyle::card;
  panel.cooldown_ids = {"dash"};
  profile.panels.push_back(std::move(panel));
  return profile;
}

[[nodiscard]] daemon::DisplayRoute overlay_route(FakeDisplay &provider) {
  return {.windows = &provider,
          .monitors = &provider,
          .overlay = &provider,
          .capture = {},
          .native_overlay = true,
          .inputs = {}};
}

void overlay_retry_lifecycle() {
  layerc::test::TempDirectory temp("layerc-overlay-retry");
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  if (!expect(config.applied() && data.applied(),
              "overlay-retry roots are isolated"))
    return;
  const auto roots = platform::profile_store_roots();
  if (!expect(platform::ensure_profile_store_roots(roots),
              "overlay-retry store roots exist"))
    return;
  const auto make_stored = [&](core::GameProfile profile,
                               std::string_view filename) {
    platform::StoredProfile stored{roots.config_profiles / filename,
                                   std::move(profile)};
    const bool written = platform::write_profile_bundle(
                             stored.path, stored.profile, std::nullopt,
                             std::nullopt).ok();
    return std::pair{std::move(stored), written};
  };
  const auto add_status = [](daemon::LinuxStatusSnapshot &status) {
    status.capabilities.push_back(
        {.capability = runtime::Capability::overlay,
         .availability = runtime::CapabilityAvailability::inactive,
         .reasons = {},
         .diagnostics = {}});
  };
  const auto overlay_row = [](const daemon::LinuxStatusSnapshot &status)
      -> const runtime::CapabilityStatus * {
    const auto found = std::ranges::find(
        status.capabilities, runtime::Capability::overlay,
        &runtime::CapabilityStatus::capability);
    return found == status.capabilities.end() ? nullptr : &*found;
  };
  const auto has_failed_panel = [](const daemon::OverlayService &overlay,
                                   const std::string_view panel,
                                   const std::string_view profile) {
    const auto reasons = overlay.unavailable();
    const auto expected = "overlay: could not show panel " + std::string(panel) +
                          " for profile " + std::string(profile);
    return std::ranges::find(reasons, expected) != reasons.end();
  };
  platform::WindowSnapshot window;
  window.window = 9;
  window.executable_name = "fixture.exe";
  window.width = 800;
  window.height = 600;
  window.active = true;

  {
    auto [stored, written] = make_stored(overlay_retry_profile("overlay-retry"),
                                         "overlay-retry.json");
    if (!expect(written, "retry profile is stored")) return;
    FakeDisplay provider;
    provider.scan.windows.push_back(window);
    provider.scan.active = 9;
    const auto route = overlay_route(provider);
    daemon::SessionEngine engine;
    runtime::DaemonKernel kernel(engine.runtime_services());
    daemon::LinuxStatusSnapshot status;
    add_status(status);
    std::vector<std::string> diagnostics;
    engine.refresh(std::span{&stored, 1}, true, route, kernel, status,
                   diagnostics);
    if (!expect(engine.session_count() == 1 &&
                    kernel.demand().required(runtime::Capability::overlay),
                "card panel fixture admits one demanded overlay session")) {
      kernel.quiesce();
      return;
    }
    const auto now = core::MonotonicClock::now();
    provider.fail_show = true;
    (void)engine.render(now);
    auto *overlay = engine.runtime_services()[static_cast<std::size_t>(
        runtime::Capability::overlay)];
    auto &overlay_service = static_cast<daemon::OverlayService &>(*overlay);
    engine.refresh_status(kernel, status);
    const auto due = engine.next_deadline(now);
    const auto *row = overlay_row(status);
    expect(provider.shows == 1 && provider.visible.empty() &&
               overlay_service.window_count() == 0,
           "a failed demanded show creates no overlay window");
    expect(row != nullptr &&
               row->availability == runtime::CapabilityAvailability::degraded &&
               !row->diagnostics.empty() &&
               has_failed_panel(overlay_service, "main", "overlay-retry"),
           "failed show is visible in overlay status");
    if (!expect(due == std::optional{now + daemon::overlay_repaint_interval},
                "failed show arms an absolute retry deadline")) {
      kernel.quiesce();
      return;
    }
    (void)engine.render(now + std::chrono::milliseconds{1});
    expect(provider.shows == 1 &&
               engine.next_deadline(now + std::chrono::milliseconds{1}) == due,
           "pre-deadline render neither retries nor moves the absolute deadline");
    diagnostics.clear();
    engine.refresh(std::span{&stored, 1}, false, route, kernel, status,
                   diagnostics);
    expect(engine.next_deadline(now + std::chrono::milliseconds{1}) == due,
           "target refresh preserves the same-window retry deadline");

    provider.fail_show = false;
    (void)engine.render(*due);
    engine.refresh_status(kernel, status);
    const auto repaint = *due + daemon::overlay_repaint_interval;
    row = overlay_row(status);
    expect(provider.visible.size() == 1 && row != nullptr &&
               row->availability == runtime::CapabilityAvailability::active &&
               row->diagnostics.empty(),
           "successful retry maps the panel and clears its fault");
    expect(engine.next_deadline(*due) == std::optional{repaint},
           "a shown panel schedules its normal repaint deadline");

    provider.fail_show = true;
    (void)engine.render(repaint);
    engine.refresh_status(kernel, status);
    expect(provider.visible.empty() &&
               has_failed_panel(overlay_service, "main", "overlay-retry") &&
               engine.next_deadline(repaint) ==
                   std::optional{repaint + daemon::overlay_repaint_interval},
           "a failed repaint starts another demanded-only retry");

    auto profile = stored.profile;
    profile.panels.front().style = core::PanelStyle::bar;
    profile.panels.front().bar.hide_when_ready = true;
    stored.profile = profile;
    if (!expect(platform::write_profile_bundle(stored.path, profile,
                                               std::nullopt, std::nullopt).ok(),
                "ready bar profile is stored")) {
      kernel.quiesce();
      return;
    }
    const auto before_hidden = provider.shows;
    diagnostics.clear();
    engine.refresh(std::span{&stored, 1}, true, route, kernel, status,
                   diagnostics);
    const auto hidden_at = core::MonotonicClock::now();
    (void)engine.render(hidden_at);
    engine.refresh_status(kernel, status);
    row = overlay_row(status);
    expect(provider.shows == before_hidden && provider.visible.empty() &&
               row != nullptr && row->diagnostics.empty() &&
               !engine.next_deadline(hidden_at),
           "intentional hide cancels a failed show without fault or deadline");

    profile.panels.front().style = core::PanelStyle::card;
    stored.profile = profile;
    if (!expect(platform::write_profile_bundle(stored.path, profile,
                                               std::nullopt, std::nullopt).ok(),
                "demanded card profile is restored")) {
      kernel.quiesce();
      return;
    }
    diagnostics.clear();
    engine.refresh(std::span{&stored, 1}, true, route, kernel, status,
                   diagnostics);
    provider.fail_show = true;
    (void)engine.render(core::MonotonicClock::now());
    expect(has_failed_panel(overlay_service, "main", "overlay-retry"),
           "restored demanded card arms a new failure retry");
    profile.enabled = false;
    stored.profile = profile;
    if (!expect(platform::write_profile_bundle(stored.path, profile,
                                               std::nullopt, std::nullopt).ok(),
                "disabled profile is stored")) {
      kernel.quiesce();
      return;
    }
    diagnostics.clear();
    engine.refresh(std::span{&stored, 1}, true, route, kernel, status,
                   diagnostics);
    expect(engine.session_count() == 0 && !engine.next_deadline(
               core::MonotonicClock::now()),
           "profile removal disarms the failed overlay target");
    kernel.quiesce();
    (void)engine.render(core::MonotonicClock::now() +
                        std::chrono::seconds{10});
    expect(!engine.next_deadline(core::MonotonicClock::now()) &&
               provider.visible.empty(),
           "quiescence leaves no retry deadline or overlay resurrection");
  }

  {
    auto [stored, written] = make_stored(overlay_retry_profile("overlay-swap"),
                                         "overlay-swap.json");
    if (!expect(written, "backend-swap profile is stored")) return;
    FakeDisplay first;
    FakeDisplay second;
    first.scan.windows.push_back(window);
    second.scan = first.scan;
    const auto first_route = overlay_route(first);
    const auto second_route = overlay_route(second);
    daemon::SessionEngine engine;
    runtime::DaemonKernel kernel(engine.runtime_services());
    daemon::LinuxStatusSnapshot status;
    add_status(status);
    std::vector<std::string> diagnostics;
    engine.refresh(std::span{&stored, 1}, true, first_route, kernel, status,
                   diagnostics);
    first.fail_show = true;
    const auto now = core::MonotonicClock::now();
    (void)engine.render(now);
    engine.refresh_status(kernel, status);
    expect(engine.next_deadline(now) ==
               std::optional{now + daemon::overlay_repaint_interval},
           "first backend holds a failed-show retry");
    diagnostics.clear();
    engine.refresh(std::span{&stored, 1}, false, second_route, kernel, status,
                   diagnostics);
    (void)engine.render(now);
    engine.refresh_status(kernel, status);
    const auto *row = overlay_row(status);
    expect(second.shows == 1 && second.visible.size() == 1 && row != nullptr &&
               row->availability == runtime::CapabilityAvailability::active &&
               row->diagnostics.empty(),
           "backend replacement clears the retry and shows immediately");
    second.fail_show = true;
    const auto failed_at = now + daemon::overlay_repaint_interval;
    (void)engine.render(failed_at);
    expect(engine.next_deadline(failed_at) ==
               std::optional{failed_at + daemon::overlay_repaint_interval},
           "quiescence fixture starts with an armed failed-show retry");
    const auto shows_before_quiesce = second.shows;
    kernel.quiesce();
    const auto after_quiesce = failed_at + std::chrono::seconds{10};
    (void)engine.render(after_quiesce);
    expect(!engine.next_deadline(after_quiesce) && second.visible.empty() &&
               second.shows == shows_before_quiesce,
           "quiescence cancels an armed retry without overlay resurrection");
  }

  {
    auto profile = overlay_retry_profile("overlay-multi");
    auto second_panel = profile.panels.front();
    second_panel.id = "secondary";
    second_panel.name = "Secondary";
    profile.panels.push_back(second_panel);
    auto [stored, written] = make_stored(std::move(profile),
                                         "overlay-multi.json");
    if (!expect(written, "multi-panel profile is stored")) return;
    FakeDisplay provider;
    provider.scan.windows.push_back(window);
    provider.scan.active = 9;
    provider.fail_show = true;
    provider.failed_panel = "main";
    const auto route = overlay_route(provider);
    daemon::SessionEngine engine;
    runtime::DaemonKernel kernel(engine.runtime_services());
    daemon::LinuxStatusSnapshot status;
    add_status(status);
    std::vector<std::string> diagnostics;
    engine.refresh(std::span{&stored, 1}, true, route, kernel, status,
                   diagnostics);
    const auto now = core::MonotonicClock::now();
    (void)engine.render(now);
    engine.refresh_status(kernel, status);
    auto *service = engine.runtime_services()[static_cast<std::size_t>(
        runtime::Capability::overlay)];
    auto &overlay_service = static_cast<daemon::OverlayService &>(*service);
    const auto *row = overlay_row(status);
    expect(provider.shows == 2 && provider.visible.size() == 1 &&
               provider.visible.begin()->second == "secondary",
           "a healthy panel shows while the other demanded panel fails");
    expect(row != nullptr &&
               row->availability == runtime::CapabilityAvailability::degraded &&
               !row->diagnostics.empty() &&
               has_failed_panel(overlay_service, "main", "overlay-multi"),
           "a healthy show cannot clear another panel's retry diagnostic");
    const auto due = engine.next_deadline(now);
    expect(due == std::optional{now + daemon::overlay_repaint_interval},
           "multi-panel failure retains the failed target retry deadline");
    provider.fail_show = false;
    if (due) (void)engine.render(*due);
    engine.refresh_status(kernel, status);
    row = overlay_row(status);
    expect(provider.shows == 4 && provider.visible.size() == 2 && row != nullptr &&
               row->availability == runtime::CapabilityAvailability::active &&
               row->diagnostics.empty(),
           "retry recovers the failed panel and clears its fault");
    kernel.quiesce();
  }
}

void provider_free_ownership() {
  daemon::SessionEngine engine;
  const auto services = engine.runtime_services();
  for (const auto capability : {runtime::Capability::input, runtime::Capability::automation,
                                runtime::Capability::overlay, runtime::Capability::visual})
    expect(services[static_cast<std::size_t>(capability)] != nullptr,
           "runtime ownership does not depend on a display provider");
  const auto needs = engine.input_needs();
  expect(!needs.keyboard && !needs.mouse && !needs.gamepad,
         "a provider-free engine needs no passive input devices");
  runtime::DaemonKernel kernel(services);
  daemon::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  engine.refresh({}, true, {}, kernel, status, diagnostics);
  expect(engine.session_count() == 0 && !engine.next_deadline(core::MonotonicClock::now()),
         "a provider-free engine stays idle without a polling deadline");
  kernel.quiesce();
}
class FakeSource final : public daemon::PassiveInputSource {
public:
  explicit FakeSource(std::string_view name) : name_(name) {}
  bool available{true}, open_ok{true}, opened{};
  unsigned opens{}, closes{}, polls{};
  std::string fault;
  std::vector<std::string> missing;
  bool input_available() const noexcept override { return available; }
  bool open_input(std::string &error) override {
    ++opens;
    if (!open_ok) { error = "input: " + std::string(name_) + " failed"; return false; }
    opened = true;
    return true;
  }
  void close_input() noexcept override { if (opened) ++closes; opened = false; }
  bool input_open() const noexcept override { return opened; }
  std::vector<std::string> input_unavailable(daemon::InputDeviceNeeds) const override {
    return fault.empty() ? missing : std::vector<std::string>{fault};
  }
  runtime::RuntimeResourceStats input_resources() const noexcept override {
    runtime::RuntimeResourceStats stats;
    stats.handles = opened ? 2U : 0U;
    return stats;
  }
  std::string_view input_name() const noexcept override { return name_; }
  daemon::PassiveInputPoll poll_input(std::span<const int>) override {
    ++polls;
    daemon::PassiveInputPoll result;
    result.events.push_back({{core::InputDevice::keyboard, 18, 0}, true});
    return result;
  }
  std::string_view input_fault() const noexcept override { return fault; }
private:
  std::string_view name_;
};

void passive_service_contracts() {
  layerc::test::TempDirectory temp("layerc-passive-service");
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  const auto roots = platform::profile_store_roots();
  if (!expect(platform::ensure_profile_store_roots(roots), "private service fixture roots exist")) return;
  core::GameProfile profile;
  profile.id = "service";
  profile.name = "Service";
  profile.game.global = true;
  profile.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  profile.bindings.push_back({.cooldown_id = "dash",
      .chord = {{{core::InputDevice::keyboard, 18, 0}}}});
  const platform::StoredProfile stored{roots.config_profiles / "service.json", profile};
  platform::WindowScan scan;
  platform::WindowSnapshot window;
  window.window = 7;
  window.active = true;
  scan.windows.push_back(window);
  scan.active = 7;
  daemon::SessionRegistry registry;
  registry.configure(std::span{&stored, 1});
  registry.refresh(scan);
  if (!expect(registry.session_count() == 1, "a global binding admits one session")) return;
  const auto demand = runtime::calculate_capability_demand(registry.session_requests(), false);
  const runtime::CapabilityDemand none;
  const auto ok = [](const runtime::ServiceReconcileResult &result) {
    return result.status == runtime::ServiceReconcileStatus::ok;
  };
  FakeSource first("evdev"), second("XI2");
  daemon::InputService service(registry);
  const std::array<daemon::PassiveInputSource *, 2> ordered{&first, &second};
  service.use_sources(ordered);
  expect(!ok(service.reconcile(none)) && first.opens == 0 && second.opens == 0,
         "no demand opens no passive source");
  expect(ok(service.reconcile(demand)) && first.opened && second.opens == 0 &&
             service.attached() == &first,
         "the first source that opens serves and the fallback stays closed");
  first.missing = {"evdev_no_mouse"};
  expect(service.unavailable() == first.missing, "the attached source reports its own availability");
  const auto dispatched = service.poll({});
  expect(first.polls == 1 && second.polls == 0 && dispatched.events == 1 &&
             dispatched.triggered == std::vector<std::string>{"dash"},
         "poll drains only the attached source and feeds the shared session");
  expect(service.resources().handles == 2, "resources sum over the held sources");
  service.use_sources(ordered);
  expect(first.opened && first.closes == 0, "an unchanged source set stays open");
  service.quiesce();
  expect(!first.opened && !second.opened && first.closes == 1 && service.resources().handles == 0,
         "quiesce closes every source");
  first.open_ok = false;
  first.fault = "evdev_permission_denied: add the user to the 'input' group";
  expect(ok(service.reconcile(demand)) && first.opens == 2 && second.opened &&
             service.attached() == &second,
         "a failed first source falls back to the next source in order");
  second.missing = {"input: XI2 lacks a keyboard"};
  expect(service.unavailable() == second.missing,
         "the attached fallback's availability wins over another source's fault");
  service.quiesce();
  second.open_ok = false;
  const auto refused = service.reconcile(demand);
  expect(!ok(refused) && refused.reason == "input: XI2 failed",
         "when every source fails the last error is reported");
  expect(service.unavailable() == std::vector<std::string>{first.fault},
         "with nothing attached the first source's fault remains visible");
  first.fault.clear();
  expect(service.unavailable() == std::vector<std::string>{"input: XI2 source is not attached"},
         "without a fault the last source in order is named");
  second.open_ok = true;
  expect(ok(service.reconcile(demand)) && service.attached() == &second, "the fallback reopens");
  const std::array<daemon::PassiveInputSource *, 1> only{&first};
  service.use_sources(only);
  expect(!second.opened && service.attached() == nullptr, "dropping an open source closes it");
  service.use_sources({});
  expect(service.reconcile(demand).reason == "input: no X display for the input service",
         "with no source the existing reason is unchanged");
  registry.stop_sessions(core::MonotonicClock::now());
}

// F02: capture is resolved per target. A window that cannot be captured reports
// one visible reason and never contributes a sampling deadline, and a route
// that gains the missing factory starts capturing without restarting anything.
void unpollable_capture_lifecycle() {
  layerc::test::TempDirectory temp("layerc-unpollable-capture");
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME", temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  const auto roots = platform::profile_store_roots();
  if (!expect(config.applied() && data.applied() &&
                  platform::ensure_profile_store_roots(roots),
              "private capture fixture roots exist"))
    return;
  core::GameProfile profile;
  profile.id = "capture";
  profile.name = "Capture";
  profile.game.executable_name = "fixture.exe";
  profile.cooldowns.push_back({.id = "dash", .name = "Dash",
                               .duration = std::chrono::seconds(5)});
  profile.bindings.push_back({.cooldown_id = "dash",
                              .chord = {{{core::InputDevice::keyboard, 18, 0}}}});
  core::VisualProfile visual;
  visual.profile_id = profile.id;
  visual.sample_rate_hz = 10;
  core::PixelRule ready;
  ready.id = "ready";
  ready.name = "Ready";
  visual.observations.push_back(ready);
  visual.triggers.push_back({.cooldown_id = "dash",
                             .observation_id = "ready",
                             .keep_input = true});
  // No panel and no timer: visual sampling is the only deadline source here.
  const platform::StoredProfile stored{roots.config_profiles / "capture.json",
                                       profile};
  if (!expect(platform::write_profile_bundle(stored.path, profile, visual).ok(),
              "the capture fixture and its visual sidecar are stored"))
    return;

  FakeDisplay provider;
  platform::WindowSnapshot window;
  window.window = 9;
  window.executable_name = "fixture.exe";
  window.width = 800;
  window.height = 600;
  window.active = true;
  provider.scan.windows.push_back(window);
  provider.scan.active = 9;
  daemon::DisplayRoute route{.windows = &provider,
                             .monitors = &provider,
                             .capture = {.native = &provider,
                                         .native_windows = true},
                             .inputs = {&provider}};
  daemon::SessionEngine engine;
  runtime::DaemonKernel kernel(engine.runtime_services());
  daemon::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  engine.refresh(std::span{&stored, 1}, true, route, kernel, status, diagnostics);
  if (!expect(engine.session_count() == 1 && provider.captures == 1,
              "a native route captures the admitted window"))
    return;

  // The compositor now identifies the window as XWayland, but this route has
  // no X11 factory.
  provider.xid = 23;
  provider.captures = 0;
  diagnostics.clear();
  engine.refresh(std::span{&stored, 1}, false, route, kernel, status, diagnostics);
  const std::string xwayland_reason =
      "visual visual: window 9 is an XWayland window but X11 capture is "
      "unavailable";
  const auto now = core::MonotonicClock::now();
  const auto unpollable = engine.next_deadline(now);
  expect(provider.captures == 0 && engine.session_count() == 1 &&
             std::ranges::find(diagnostics, xwayland_reason) !=
                 diagnostics.end() &&
             (!unpollable.has_value() || *unpollable > now),
         "an XWayland window without X11 capture reports its reason and "
         "contributes no sampling deadline");
  const core::InputEvent press{{core::InputDevice::keyboard, 18, 0}, true};
  daemon::DisplayDispatch dispatched;
  engine.feed_input(std::span{&press, 1}, dispatched);

  expect(dispatched.triggered == std::vector<std::string>{"dash"},
         "an uncapturable visual target leaves input and automation working");

  // The X11 factory appears: the same engine resolves capture for the target.
  FakeDisplay x11_factory;
  auto routed = route;
  routed.capture.x11 = &x11_factory;
  diagnostics.clear();
  engine.refresh(std::span{&stored, 1}, false, routed, kernel, status,
                 diagnostics);
  const auto captured = engine.next_deadline(now);

  expect(x11_factory.captures == 1 &&
             std::ranges::find(diagnostics, xwayland_reason) ==
                 diagnostics.end() &&
             captured.has_value(),
         "a provider transition creates the missing capture and restores the "
         "visual deadline");
  (void)engine.render(now);
  const auto sampled = engine.next_deadline(now);
  expect(sampled.has_value() && *sampled > now,
         "a captured target schedules its next sample instead of a due loop");

  // A factory that refuses the capture reports the same class of reason and
  // the deadline property holds for it too.
  FakeDisplay refusing;
  refusing.refuse_capture = true;
  const daemon::DisplayRoute refusing_route{.windows = &provider,
                                           .monitors = &provider,
                                           .capture = {.native = &refusing,
                                                       .native_windows = true},
                                           .inputs = {&provider}};
  std::vector<std::string> refused_diagnostics;
  engine.refresh(std::span{&stored, 1}, false, refusing_route, kernel, status,
                 refused_diagnostics);
  const auto refused_deadline = engine.next_deadline(now);
  expect(refusing.captures != 0 &&
             std::ranges::find(
                 refused_diagnostics,
                 "visual visual: capture could not be created for window 9") !=
                 refused_diagnostics.end() &&
             (!refused_deadline.has_value() || *refused_deadline > now),
         "a refused capture reports its reason and arms no sampling deadline");
  kernel.quiesce();
  engine.stop_sessions(core::MonotonicClock::now());
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(neutral_session_engine),
                 LAYERC_TEST_CASE(passive_service_contracts),
                 LAYERC_TEST_CASE(provider_loss_lifecycle),
                 LAYERC_TEST_CASE(overlay_retry_lifecycle),
                 LAYERC_TEST_CASE(provider_free_ownership),
                 LAYERC_TEST_CASE(unpollable_capture_lifecycle))
