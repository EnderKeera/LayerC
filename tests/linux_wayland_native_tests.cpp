// Native Wayland providers against a disposable headless sway session
// (tools/wayland-test-session.sh). The product adapters share one connection;
// a second test-only client provides the "game" window, screen reads and a
// virtual pointer.

#include "display_adapters.hpp"
#include "wayland_connection.hpp"
#include "wayland_monitors.hpp"
#include "wayland_overlay.hpp"
#include "wayland_test_client.hpp"
#include "wayland_toplevels.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace {
using layerc::test::expect;
using namespace layerc::platform;

constexpr std::uint32_t fixture_rgb = 0x2040A0;
constexpr std::uint32_t panel_rgb = 0xE01890;
constexpr char fixture_title[] = "LayerC Fixture";
constexpr char fixture_app_id[] = "io.github.EnderKeera.LayerC.Fixture";

void require_session() {
  if (!std::getenv("WAYLAND_DISPLAY") || !std::getenv("SWAYSOCK")) layerc::test::skip();
}

// The product side: one connection plus every native provider, released by
// the connection hook before its display goes away.
struct Providers {
  WaylandMonitors monitors;
  WaylandToplevels toplevels;
  WaylandOverlay overlay;
  // Declared last so it is destroyed first: its release hook still sees
  // live providers.
  std::unique_ptr<WaylandConnection> connection = std::make_unique<WaylandConnection>();

  bool open() {
    connection->set_release_hook([this] {
      overlay.release();
      toplevels.release();
      monitors.release();
    });
    if (!connection->open(layerc::core::Milliseconds{2000})) return false;
    sync();
    return true;
  }
  void sync() {
    monitors.sync(*connection);
    toplevels.sync(*connection);
    overlay.sync(*connection);
    connection->flush();
  }
  void pump() {
    if (!connection) return;
    if (connection->dispatch()) sync();
    (void)connection->has_pending_events();
    overlay.repaint();
    connection->flush();
  }
};

const Monitor *find_monitor(const MonitorScan &scan, std::string_view name) {
  const auto it = std::ranges::find(scan.monitors, name, &Monitor::name);
  return it == scan.monitors.end() ? nullptr : &*it;
}

const WindowSnapshot *find_window(const WindowScan &scan, std::string_view title) {
  const auto it = std::ranges::find(scan.windows, title, &WindowSnapshot::title);
  return it == scan.windows.end() ? nullptr : &*it;
}

OverlayDisplayList solid_panel(int x, int y, int width, int height) {
  OverlayDisplayList list;
  list.bounds = {x, y, width, height};
  list.ops.push_back({OverlayOpKind::fill_rect, {0, 0, width, height}, panel_rgb, {}, 0});
  return list;
}

void monitors_report_logical_layout() {
  require_session();
  Providers providers;
  if (!expect(providers.open(), "connect to the disposable compositor")) return;
  const auto pump = [&] { providers.pump(); };
  const bool ready = layerc::test::wait_until(pump, [&] {
    // Wait for the final configuration, not just the names: the scale-2
    // rule may land a moment after the second output appears.
    const auto scan = providers.monitors.scan();
    const auto *second = find_monitor(scan, "HEADLESS-2");
    return find_monitor(scan, "HEADLESS-1") && second && second->buffer_scale == 2 &&
           second->x == 1280 && second->width == 640;
  });
  if (!expect(ready, "both headless outputs are reported")) return;
  const auto scan = providers.monitors.scan();
  const auto *first = find_monitor(scan, "HEADLESS-1");
  const auto *second = find_monitor(scan, "HEADLESS-2");
  expect(scan.available && scan.warning.empty(), "xdg-output monitors are authoritative");
  expect(first->x == 0 && first->y == 0 && first->width == 1280 && first->height == 720 &&
             first->buffer_scale == 1 && first->dpi == baseline_dpi,
         "scale-1 output keeps its mode as logical size at 96 dpi");
  expect(second->x == 1280 && second->y == 0 && second->width == 640 &&
             second->height == 360 && second->buffer_scale == 2 && second->dpi == baseline_dpi,
         "scale-2 output reports logical size and an integer buffer scale");
  expect(providers.monitors.output("HEADLESS-2") != nullptr,
         "scanned monitors resolve to their output proxy");
}

void toplevels_track_fixture_window() {
  require_session();
  Providers providers;
  layerc::test::WaylandTestClient client;
  std::string error;
  if (!expect(providers.open(), "connect product adapters") ||
      !expect(client.connect(error), "connect fixture client: " + error))
    return;
  if (!expect(client.show_window(fixture_title, fixture_app_id, "HEADLESS-1", fixture_rgb),
              "map the fullscreen fixture window"))
    return;
  const auto pump = [&] {
    client.pump();
    providers.pump();
  };
  const bool tracked = layerc::test::wait_until(pump, [&] {
    const auto *window = find_window(providers.toplevels.scan(providers.monitors), fixture_title);
    return window && window->active && window->fullscreen;
  });
  if (!expect(tracked, "foreign toplevel reports the fixture as active fullscreen")) {
    for (const auto &window : providers.toplevels.scan(providers.monitors).windows)
      std::fprintf(stderr, "  toplevel id=%llu title='%s' app='%s' active=%d fullscreen=%d hidden=%d rect=%d,%d %dx%d\n",
                   static_cast<unsigned long long>(window.window), window.title.c_str(),
                   window.class_name.c_str(), window.active, window.fullscreen, window.hidden,
                   window.x, window.y, window.width, window.height);
    return;
  }
  const auto scan = providers.toplevels.scan(providers.monitors);
  const auto *window = find_window(scan, fixture_title);
  expect(scan.authoritative && scan.warning.empty() && scan.active == window->window &&
             window->window != 0,
         "the scan is authoritative with a stable nonzero id");
  expect(window->class_name == fixture_app_id && window->instance_name == fixture_app_id &&
             !window->pid && window->executable_name.empty(),
         "app_id maps to class/instance; PID and executable stay unavailable");
  const auto id = window->window;
  if (window->width == 0) {
    // Sway 1.10 sends no output_enter: with two monitors the geometry stays
    // unknown instead of guessing. Unplugging one makes it unambiguous.
    expect(window->x == 0 && window->y == 0 && window->height == 0,
           "without output_enter a multi-monitor geometry stays empty");
    expect(std::system("swaymsg -q output HEADLESS-2 unplug") == 0, "unplug the second output");
  }
  const bool placed = layerc::test::wait_until(pump, [&] {
    const auto *current = find_window(providers.toplevels.scan(providers.monitors), fixture_title);
    return current && current->x == 0 && current->y == 0 && current->width == 1280 &&
           current->height == 720 && !current->hidden;
  });
  expect(placed && providers.monitors.scan().monitors.size() <= 2,
         "geometry is the entered output, or the only monitor");
  (void)providers.toplevels.take_changed();
  client.close_window();
  const bool closed = layerc::test::wait_until(pump, [&] {
    return !find_window(providers.toplevels.scan(providers.monitors), fixture_title);
  });
  expect(closed && providers.toplevels.take_changed(), "a closed toplevel leaves the scan");
  for (const auto &remaining : providers.toplevels.scan(providers.monitors).windows)
    expect(remaining.window != id, "closed ids are not reused while tracked");
}

void overlay_is_mapped_and_click_through() {
  require_session();
  Providers providers;
  layerc::test::WaylandTestClient client;
  std::string error;
  if (!expect(providers.open(), "connect product adapters") ||
      !expect(client.connect(error), "connect fixture client: " + error))
    return;
  expect(providers.overlay.unavailable().empty(), "sway advertises layer-shell, compositor and shm");
  if (!expect(client.show_window(fixture_title, fixture_app_id, "HEADLESS-1", fixture_rgb),
              "map the fullscreen fixture window"))
    return;
  const auto pump = [&] {
    client.pump();
    providers.pump();
  };
  if (!expect(layerc::test::wait_until(pump, [&] {
                return find_monitor(providers.monitors.scan(), "HEADLESS-2") != nullptr;
              }),
              "outputs ready"))
    return;
  const auto scan = providers.monitors.scan();
  const auto *first = find_monitor(scan, "HEADLESS-1");
  const auto *second = find_monitor(scan, "HEADLESS-2");
  const auto panel = solid_panel(100, 100, 200, 60);
  const auto scaled = solid_panel(1300, 20, 100, 40);
  const bool presented =
      providers.overlay.present("panel", panel, providers.monitors.output("HEADLESS-1"), *first) &&
      providers.overlay.present("scaled", scaled, providers.monitors.output("HEADLESS-2"), *second);
  if (!expect(presented, "present panels on both outputs")) return;
  const bool mapped = layerc::test::wait_until(pump, [&] {
    return providers.overlay.mapped("panel") && providers.overlay.mapped("scaled");
  });
  if (!expect(mapped, "layer surfaces are configured and committed")) return;

  // Screen pixels prove the panel is above the fullscreen game window.
  std::optional<layerc::test::WaylandPixels> pixels;
  const bool painted = layerc::test::wait_until(pump, [&] {
    pixels = client.capture("HEADLESS-1");
    return pixels && pixels->at(150, 130) == panel_rgb;
  });
  if (!expect(painted, "panel pixels are visible over the fullscreen window")) return;
  expect(pixels->at(50, 50) == fixture_rgb && pixels->at(320, 130) == fixture_rgb,
         "the panel covers only its own rect");
  std::optional<layerc::test::WaylandPixels> scaled_pixels;
  const bool crisp = layerc::test::wait_until(pump, [&] {
    scaled_pixels = client.capture("HEADLESS-2");
    // Logical (1310,30) on a scale-2 output starting at x=1280: device (60,60).
    return scaled_pixels && scaled_pixels->at(60, 60) == panel_rgb;
  });
  expect(crisp && scaled_pixels->at(39, 39) != panel_rgb && scaled_pixels->at(40, 40) == panel_rgb &&
             scaled_pixels->at(239, 119) == panel_rgb && scaled_pixels->at(240, 120) != panel_rgb,
         "scale-2 panels fill exactly their logical rect in device pixels");

  // A click on the panel must reach the window beneath it.
  const auto press = client.click(150, 130, 1920, 720);
  expect(press && press->on_fixture && static_cast<int>(press->x) == 150 &&
             static_cast<int>(press->y) == 130,
         "the overlay's empty input region passes the click to the game window");

  // Headless sway composites its software cursor into captured frames, so
  // sample away from the click position.
  providers.overlay.remove("panel");
  const bool removed = layerc::test::wait_until(pump, [&] {
    pixels = client.capture("HEADLESS-1");
    return pixels && pixels->at(280, 150) == fixture_rgb;
  });
  if (!expect(removed && providers.overlay.surface_count() == 1, "removing a panel unmaps it"))
    std::fprintf(stderr, "  surfaces=%zu pixel=%06x\n", providers.overlay.surface_count(),
                 pixels ? pixels->at(280, 150) : 0u);
}

void overlay_moves_and_survives_connection_release() {
  require_session();
  layerc::test::WaylandTestClient client;
  std::string error;
  if (!expect(client.connect(error), "connect fixture client: " + error)) return;
  auto providers = std::make_unique<Providers>();
  if (!expect(providers->open(), "connect product adapters")) return;
  const auto pump = [&] {
    client.pump();
    providers->pump();
  };
  if (!expect(layerc::test::wait_until(pump, [&] {
                return find_monitor(providers->monitors.scan(), "HEADLESS-1") != nullptr;
              }),
              "outputs ready"))
    return;
  const auto first = *find_monitor(providers->monitors.scan(), "HEADLESS-1");
  auto *output = providers->monitors.output("HEADLESS-1");
  if (!expect(providers->overlay.present("panel", solid_panel(10, 10, 50, 20), output, first),
              "present"))
    return;
  (void)layerc::test::wait_until(pump, [&] { return providers->overlay.mapped("panel"); });
  if (!expect(providers->overlay.present("panel", solid_panel(400, 300, 80, 30), output, first),
              "move and resize"))
    return;
  std::optional<layerc::test::WaylandPixels> pixels;
  const bool moved = layerc::test::wait_until(pump, [&] {
    pixels = client.capture("HEADLESS-1");
    return pixels && pixels->at(440, 315) == panel_rgb && pixels->at(20, 15) != panel_rgb;
  });
  expect(moved, "a moved and resized panel is repainted after its configure");
  // Destroying the connection first runs the release hook while the display
  // is valid; the adapters are then inert and safe to destroy.
  providers->connection.reset();
  expect(providers->overlay.surface_count() == 0 && providers->monitors.scan().monitors.empty() &&
             providers->toplevels.scan(providers->monitors).windows.empty(),
         "release drops every provider proxy");
  const bool gone = layerc::test::wait_until([&] { client.pump(); }, [&] {
    pixels = client.capture("HEADLESS-1");
    return pixels && pixels->at(440, 315) != panel_rgb;
  });
  expect(gone, "the compositor unmaps panels of a disconnected client");
}

// The daemon seam end to end without XWayland: the active foreign toplevel
// gets a session and a layer-shell panel on screen, and status names the
// native providers.
void daemon_routes_native_providers() {
  require_session();
  const layerc::test::ScopedEnvironment backend("LAYERC_DISPLAY_BACKEND", "wayland");
  layerc::test::WaylandTestClient client;
  std::string error;
  if (!expect(client.connect(error), "connect fixture client: " + error) ||
      !expect(client.show_window(fixture_title, fixture_app_id, "HEADLESS-1", fixture_rgb),
              "map the fullscreen fixture window"))
    return;
  // Foreign toplevels carry no PID, so an executable-matched profile cannot
  // attach; the (test-only) global profile follows the active toplevel.
  const std::string json =
      std::string{R"json({"schemaVersion":1,"id":"fixture","name":"Fixture","game":{"global":true)json"} +
      R"json(},"cooldowns":[{"id":"dash","name":"Dash","durationMs":60000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":18}]}]})json";
  auto parsed = layerc::core::parse_profile_json(json);
  auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (!expect(profile != nullptr, "fixture profile parses")) {
    if (const auto *failure = std::get_if<layerc::core::ProfileError>(&parsed))
      std::fprintf(stderr, "  %s\n", failure->describe().c_str());
    return;
  }
  const std::vector<layerc::platform::StoredProfile> profiles{{"/nonexistent/fixture.json", *profile}};

  layerc::daemon::DisplayAdapters adapters;
  layerc::runtime::DaemonKernel kernel(adapters.runtime_services());
  layerc::platform::EventLoop loop;
  layerc::daemon::LinuxStatusSnapshot status;
  for (std::size_t i = 0; i < layerc::runtime::capability_count; ++i)
    status.capabilities.push_back({static_cast<layerc::runtime::Capability>(i),
                                   layerc::runtime::CapabilityAvailability::unavailable, {}, {}});
  std::vector<std::string> diagnostics;
  adapters.start(loop, status, diagnostics);
  if (!expect(status.display && status.display->session == layerc::runtime::DisplaySession::wayland,
              "the forced Wayland session is selected"))
    return;
  // Self-contained: toplevel geometry needs a single monitor (sway sends no
  // output_enter), whatever earlier cases left behind.
  (void)std::system("swaymsg -q output HEADLESS-2 unplug >/dev/null 2>&1");
  adapters.refresh(profiles, true, kernel, status, diagnostics);
  const auto pump = [&] {
    client.pump();
    if (adapters.dispatch_pending().windows_changed || adapters.has_pending_events())
      adapters.refresh(profiles, false, kernel, status, diagnostics);
    (void)adapters.render(layerc::core::MonotonicClock::now());
  };
  const bool shown = layerc::test::wait_until(pump, [&] {
    const auto rows = adapters.session_diagnostics();
    return rows.size() == 1 && rows.front().overlay_windows == 1;
  }, 5000);
  if (!expect(shown, "the fixture profile gets a session and a native overlay panel")) return;

  OverlayFrame frame;
  frame.target = {0, 0, 1280, 720};
  frame.panel = profile->panels.front();
  frame.bars.resize(1);
  const auto rect = panel_rect(frame);
  std::optional<layerc::test::WaylandPixels> pixels;
  const bool painted = layerc::test::wait_until(pump, [&] {
    pixels = client.capture("HEADLESS-1");
    return pixels && pixels->at(rect.x + rect.width / 2, rect.y + 2) != fixture_rgb &&
           pixels->at(rect.x + rect.width + 20, rect.y + rect.height + 20) == fixture_rgb;
  });
  expect(painted, "the panel is painted at its anchored rect above the game window");

  adapters.refresh_status(kernel, status);
  const auto &providers = status.display->providers;
  expect(providers.windows.name == "wlr_foreign_toplevel" &&
             providers.windows.state == layerc::runtime::DisplayProviderState::degraded,
         "windows come from foreign-toplevel, degraded for missing PID/geometry");
  expect(providers.monitors.name == "wayland_output" &&
             providers.monitors.state == layerc::runtime::DisplayProviderState::active,
         "monitors come from wl_output/xdg-output");
  expect(providers.overlay.name == "wlr_layer_shell" &&
             providers.overlay.state == layerc::runtime::DisplayProviderState::active,
         "the overlay capability is active through layer-shell");
  const auto overlay_row = std::ranges::find(status.capabilities, layerc::runtime::Capability::overlay,
                                             &layerc::runtime::CapabilityStatus::capability);
  expect(overlay_row != status.capabilities.end() &&
             overlay_row->availability == layerc::runtime::CapabilityAvailability::active,
         "Capability::overlay is active without X11 Shape");
  adapters.stop_sessions(layerc::core::MonotonicClock::now());
  kernel.quiesce();
}

} // namespace

// The toplevel case unplugs HEADLESS-2, so cases needing two outputs run
// first; the daemon case unplugs it itself when still present.
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(monitors_report_logical_layout),
                 LAYERC_TEST_CASE(overlay_is_mapped_and_click_through),
                 LAYERC_TEST_CASE(overlay_moves_and_survives_connection_release),
                 LAYERC_TEST_CASE(toplevels_track_fixture_window),
                 LAYERC_TEST_CASE(daemon_routes_native_providers))
