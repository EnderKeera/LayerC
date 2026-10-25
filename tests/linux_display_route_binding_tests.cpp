#include "display_route.hpp"
#include "evdev_route.hpp"
#include "session_engine.hpp"
#include "test_support.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
namespace core = layerc::core;
namespace daemon = layerc::daemon;
namespace platform = layerc::platform;
namespace runtime = layerc::runtime;
using Id = daemon::ProviderId;
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

class Provider final : public daemon::DisplaySource,
                       public daemon::OverlayBackend,
                       public daemon::CaptureFactory,
                       public daemon::PassiveInputSource {
public:
  explicit Provider(Id identity, std::uint64_t window) : identity_(identity) {
    platform::WindowSnapshot snapshot;
    snapshot.window = window;
    snapshot.executable_name = "binding-fixture.exe";
    snapshot.width = 800;
    snapshot.height = 600;
    snapshot.active = true;
    scan_.windows.push_back(snapshot);
    scan_.active = window;
  }
  bool windows_available{}, monitors_available{}, overlay_available{}, capture_available{}, connected{};
  mutable unsigned window_calls{}, monitor_calls{};
  unsigned shows{}, captures{}, opens{};
  bool opened{};
  bool has_windows() const override { return windows_available; }
  platform::WindowScan windows() const override { ++window_calls; return scan_; }
  bool has_monitors() const override { return monitors_available; }
  platform::MonitorScan monitors() const override { ++monitor_calls; return {}; }
  daemon::OverlayBackend *overlay() override { return overlay_available ? this : nullptr; }
  daemon::CaptureFactory *capture() override { return capture_available ? this : nullptr; }
  daemon::PassiveInputSource *input() override { return connected ? this : nullptr; }
  bool show(std::string_view, const platform::OverlayFrame &) override { ++shows; return true; }
  void hide(std::string_view) noexcept override {}
  void retain(std::span<const std::string>) noexcept override {}
  std::vector<std::string> unavailable() const override { return {}; }
  std::optional<std::uint64_t> x11_window(std::uint64_t window) const override {
    return identity_ == Id::x11_ewmh ? std::optional{window} : std::nullopt;
  }
  std::unique_ptr<daemon::VisualCaptureSession> create_capture(
      std::string_view, core::WindowId, core::CaptureQuality, std::uint8_t) override {
    ++captures;
    return std::make_unique<FakeCapture>();
  }
  bool input_available() const noexcept override { return connected; }
  std::string_view input_name() const noexcept override { return "XI2"; }
  bool open_input(std::string &) override { ++opens; opened = true; return true; }
  void close_input() noexcept override { opened = false; }
  bool input_open() const noexcept override { return opened; }
  std::vector<std::string> input_unavailable(daemon::InputDeviceNeeds) const override { return {}; }
  runtime::RuntimeResourceStats input_resources() const noexcept override { return {}; }
  void reset_work() { window_calls = monitor_calls = shows = captures = 0; }
  bool owns(std::string_view name) const {
    if (identity_ == Id::x11_ewmh) return name.starts_with("x11_");
    if (identity_ == Id::wlr_foreign_toplevel)
      return name == "wlr_foreign_toplevel" || name == "wayland_output" || name == "wlr_layer_shell";
    return name == "kwin_dbus" || name == "portal_screencast";
  }
private:
  Id identity_;
  platform::WindowScan scan_;
};

struct Case {
  const char *name;
  bool x11, wayland, native_slots, bus, transitions, evdev_failure;
};

void run_case(const Case &test) {
  layerc::test::TempDirectory temp("layerc-route-binding");
  layerc::test::ScopedEnvironment config("XDG_CONFIG_HOME", temp.path() / "config");
  layerc::test::ScopedEnvironment data("XDG_DATA_HOME", temp.path() / "data");
  const auto roots = platform::profile_store_roots();
  if (!expect(platform::ensure_profile_store_roots(roots), "private binding fixture roots exist")) return;
  core::GameProfile profile;
  profile.id = "binding";
  profile.name = "Binding";
  profile.game.executable_name = "binding-fixture.exe";
  profile.cooldowns.push_back({.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  profile.bindings.push_back({.cooldown_id = "dash", .chord = {{{core::InputDevice::keyboard, 18, 0}}}});
  profile.panels.push_back({.id = "main", .name = "Main", .bar = {}, .cooldown_ids = {"dash"}});
  core::VisualProfile visual;
  visual.profile_id = profile.id;
  core::PixelRule pixel;
  pixel.id = "ready";
  pixel.name = "Ready";
  visual.observations.push_back(pixel);
  visual.triggers.push_back({.cooldown_id = "dash", .observation_id = "ready", .keep_input = true});
  const platform::StoredProfile stored{roots.config_profiles / "binding.json", profile};
  if (!expect(platform::write_profile_bundle(stored.path, profile, visual).ok(), "store the route fixture")) return;

  Provider x11(Id::x11_ewmh, 11), wayland(Id::wlr_foreign_toplevel, 22), bus(Id::kwin_dbus, 33);
  x11.connected = test.x11;
  x11.windows_available = x11.monitors_available = x11.overlay_available = x11.capture_available = test.x11;
  wayland.windows_available = wayland.monitors_available = wayland.overlay_available = test.native_slots;
  bus.capture_available = test.bus;
  platform::EventLoop loop;
  daemon::EvdevInputRoute evdev;
  daemon::InputOptions options;
  options.selection = test.evdev_failure ? daemon::InputSelection::evdev : daemon::InputSelection::automatic;
  options.device_dir = temp.path() / "absent-devices";
  evdev.attach(loop);
  evdev.configure(options);
  const daemon::ProviderSet providers{&x11, &wayland, test.bus ? &bus : nullptr, test.x11,
                                      evdev.enabled() ? &evdev : nullptr};
  daemon::SessionEngine engine;
  runtime::DaemonKernel kernel(engine.runtime_services());
  daemon::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  const std::array all{&x11, &wayland, &bus};
  const unsigned stages = test.transitions ? 3 : 1;
  for (unsigned stage = 0; stage < stages; ++stage) {
    bus.windows_available = stage == 1;
    for (auto *provider : all) provider->reset_work();
    daemon::RouteFacts facts;
    facts.session = test.wayland ? runtime::DisplaySession::wayland : runtime::DisplaySession::x11;
    facts.x11_built = true;
    facts.x11_connected = test.x11;
    facts.ewmh = facts.randr = true;
    facts.wayland_present = test.wayland;
    facts.toplevels = wayland.has_windows();
    facts.outputs = wayland.has_monitors();
    facts.layer_shell = wayland.overlay() != nullptr;
    facts.bus_present = test.bus;
    facts.bus_windows_row = test.bus ? Id::kwin_dbus : Id::none;
    facts.reports = bus.has_windows();
    facts.input_selection = options.selection;
    facts.input_route = daemon::input_route_status(evdev, test.x11 ? &x11 : nullptr, test.wayland, engine.input_needs());
    auto plan = daemon::resolve_display_route(facts);
    auto route = daemon::bind_display_route(plan, providers);
    engine.refresh(std::span{&stored, 1}, stage == 0, route, kernel, status, diagnostics);
    if (test.evdev_failure) {
      const auto input = daemon::input_route_status(evdev, test.x11 ? &x11 : nullptr, test.wayland, engine.input_needs());
      expect(!input.evdev_attached && input.xi2_attached && !input.evdev_reasons.empty(),
             "failed evdev reconcile opens the XI2 fallback");
      facts.input_route = input;
      plan = daemon::resolve_display_route(facts);
      route = daemon::bind_display_route(plan, providers);
      engine.refresh(std::span{&stored, 1}, false, route, kernel, status, diagnostics);
      expect(plan.input.row == Id::x11_xi2 && x11.opens == 1,
             "the XI2 row names the fake that actually opened passive input");
    }
    expect(engine.session_count() == 1, std::string(test.name) + ": matching provider admits a session");
    const core::InputEvent press{{core::InputDevice::keyboard, 18, 0}, true};
    daemon::DisplayDispatch dispatch;
    engine.feed_input(std::span{&press, 1}, dispatch);
    expect(dispatch.triggered == std::vector<std::string>{"dash"}, "bound input reaches the shared session");
    (void)engine.render(core::MonotonicClock::now());
    const auto verify = [&](const daemon::RouteSlot &slot, auto count, std::string_view work) {
      for (const auto *provider : all) {
        const bool selected = slot.serving != Id::none && provider->owns(daemon::provider_name(slot.row));
        expect((count(*provider) != 0) == selected,
               std::string(test.name) + ": " + std::string(work) + " matches row " +
                   std::string(daemon::provider_name(slot.row)));
      }
    };
    verify(plan.windows, [](const Provider &provider) { return provider.window_calls; }, "windows");
    verify(plan.monitors, [](const Provider &provider) { return provider.monitor_calls; }, "monitors");
    verify(plan.overlay, [](const Provider &provider) { return provider.shows; }, "overlay");
    verify(plan.capture, [](const Provider &provider) { return provider.captures; }, "capture");
    verify(plan.input, [](const Provider &provider) { return provider.opens; }, "input");
    std::vector<daemon::PassiveInputSource *> expected_inputs;
    if (test.evdev_failure) expected_inputs.push_back(&evdev);
    if (test.x11) expected_inputs.push_back(&x11);
    expect(route.inputs == expected_inputs &&
               route.native_input == (!expected_inputs.empty() && expected_inputs.front() == &evdev),
           std::string(test.name) + ": the route binds an opted-in evdev before the XI2 fallback");
    expect(route.x11_capture == (test.x11 ? static_cast<daemon::CaptureFactory *>(&x11) : nullptr),
           "the explicit XID factory remains available alongside native capture");
    if (plan.native_windows && !test.bus) {
      expect(plan.capture.serving == Id::none && !route.capture,
             "native windows without a bus have no capture factory");
      expect(std::ranges::find(diagnostics, "visual visual: native windows have no X11 window id") != diagnostics.end(),
             "absent native capture preserves the exact visual reason");
    }
    if (test.transitions)
      expect(plan.windows.row == (stage == 1 ? Id::kwin_dbus
          : test.x11 ? Id::x11_ewmh : Id::wlr_foreign_toplevel),
          "bus reports appearing and disappearing rebind the execution identity");
  }
  kernel.quiesce();
  engine.stop_sessions(core::MonotonicClock::now());
}

void bound_execution() {
  const std::array cases{
      Case{"X11 only", true, false, false, false, false, false},
      Case{"native only / absent capture", false, true, true, false, false, false},
      Case{"mixed native slots unavailable", true, true, false, false, false, false},
      Case{"KWin reports with XWayland", true, true, true, true, true, false},
      Case{"KWin reports without X11", false, true, true, true, true, false},
      Case{"evdev failure with XI2 fallback", true, true, false, false, false, true}};
  for (const auto &test : cases) run_case(test);
}
void passive_selection_contracts() {
  Provider xi2(Id::x11_ewmh, 11), evdev(Id::evdev, 22);
  using Source = daemon::PassiveInputSource;
  const auto ordered = [&](Source *native, Source *fallback, bool preferred,
                           std::vector<Source *> expected) {
    expect(daemon::select_input_sources(native, fallback, preferred) == expected,
           "passive route source order preserves opt-in and XI2 fallback policy");
  };
  xi2.connected = true;
  ordered(nullptr, &xi2, false, {&xi2});
  ordered(nullptr, nullptr, false, {});
  ordered(nullptr, &xi2, true, {&xi2});
  ordered(&evdev, &xi2, true, {&evdev, &xi2});
  ordered(&evdev, &xi2, false, {&xi2});
  ordered(&evdev, nullptr, false, {&evdev});
  ordered(&evdev, nullptr, true, {&evdev});
  xi2.connected = false;
  ordered(nullptr, &xi2, false, {&xi2});
  ordered(&evdev, &xi2, false, {&evdev, &xi2});
  ordered(&evdev, &xi2, true, {&evdev, &xi2});
  ordered(&evdev, &evdev, true, {&evdev});
}
} // namespace
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(bound_execution),
                 LAYERC_TEST_CASE(passive_selection_contracts))
