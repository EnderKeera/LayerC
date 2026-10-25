#include "bus_providers.hpp"
#include "dbus_shell_service.hpp"
#include "gnome_bridge.hpp"
#include "portal_screencast.hpp"
#include "linux_dbus_test_support.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace {
namespace platform = layerc::platform;
namespace daemon = layerc::daemon;
namespace core = layerc::core;
namespace runtime = layerc::runtime;
namespace fixture = layerc::test::dbus;
using layerc::test::expect;

bool pump(daemon::BusProviders &providers, platform::EventLoop &loop,
          const std::vector<sd_bus *> &peers, const std::function<bool()> &done) {
  const auto end = core::MonotonicClock::now() + std::chrono::seconds{3};
  do {
    for (auto *peer : peers)
      for (int budget = 0; peer && budget < 1024 && sd_bus_process(peer, nullptr) > 0; ++budget) {}
    loop.set_deadline(providers.next_deadline());
    (void)loop.poll_once(0);
    (void)providers.dispatch(loop.ready_fds());
    if (done()) return true;
    (void)loop.poll_once(2);
  } while (core::MonotonicClock::now() < end);
  return done();
}

struct FakeGnome {
  fixture::Peer peer{fixture::peer(platform::gnome_shell_bus_name)};
  platform::BusSlot object;
  unsigned presents{}, clears{};
  std::string frame;
  FakeGnome() {
    expect(sd_bus_request_name(peer.get(), platform::gnome_bridge_bus_name, 0) >= 0,
           "extension name claimed by Shell");
    static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("Version", "u", version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("Present", "s", "", present, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_METHOD("Clear", "", "", clear, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    object = fixture::method(peer.get(), platform::gnome_bridge_bus_path,
                            platform::gnome_bridge_interface, vtable, this);
  }
  static int version(sd_bus *, const char *, const char *, const char *,
                     sd_bus_message *reply, void *, sd_bus_error *) {
    return sd_bus_message_append(reply, "u", std::uint32_t{1});
  }
  static int present(sd_bus_message *call, void *data, sd_bus_error *) {
    auto &self = *static_cast<FakeGnome *>(data);
    const char *frame{};
    if (sd_bus_message_read(call, "s", &frame) < 0) return -EINVAL;
    self.frame = frame; ++self.presents;
    return sd_bus_reply_method_return(call, "");
  }
  static int clear(sd_bus_message *call, void *data, sd_bus_error *) {
    auto &self = *static_cast<FakeGnome *>(data);
    self.frame.clear(); ++self.clears;
    return sd_bus_reply_method_return(call, "");
  }
};

struct WindowSource final : daemon::DisplaySource {
  bool has_windows() const override { return true; }
  platform::WindowScan windows() const override {
    platform::WindowScan result;
    platform::WindowSnapshot window;
    window.window = 7; window.title = "Window source";
    result.windows.push_back(std::move(window));
    return result;
  }
  bool has_monitors() const override { return false; }
  platform::MonitorScan monitors() const override { return {}; }
  daemon::OverlayBackend *overlay() override { return nullptr; }
};

std::string report(daemon::BusProviders &providers, platform::EventLoop &loop,
                   sd_bus *peer, const std::vector<sd_bus *> &peers,
                   std::string payload) {
  fixture::Result result;
  auto slot = fixture::call(peer, platform::layerc_bus_name, platform::layerc_bus_path,
      platform::layerc_shell_interface, "ReportWindows",
      [&payload](sd_bus_message *call) { return sd_bus_message_append(call, "s", payload.c_str()); },
      result);
  expect(pump(providers, loop, peers, [&] { return result.done; }), "report receives a reply");
  return result.error;
}

std::string native_report() {
  return R"({"version":1,"source":"gnome","windows":[{"id":"native","title":"Native","class":"fixture","pid":)" +
      std::to_string(::getpid()) +
      R"(,"x":10,"y":20,"width":200,"height":100,"active":true},{"id":"xwayland","title":"XWayland","x11Window":17}]})";
}

void reporting_overlay_and_idle() {
  FakeGnome shell;
  auto stranger = fixture::peer();
  const std::vector peers{shell.peer.get(), stranger.get()};
  platform::EventLoop loop;
  WindowSource window_source;
  daemon::BusProviders providers{loop};
  providers.use_window_source(&window_source);
  expect(!providers.connected() && !providers.next_deadline(), "idle creates no bus or timer");
  expect(!providers.has_windows() && providers.windows().windows.empty(),
         "a capture window source is never exposed as a bus report");
  providers.configure(true, "GNOME", false);
  providers.want_overlay(true);
  expect(pump(providers, loop, peers, [&] { return providers.overlay() != nullptr; }),
         "the bridge authenticates on the host loop");
  expect(report(providers, loop, stranger.get(), peers, native_report()) == SD_BUS_ERROR_ACCESS_DENIED,
         "provider wrapper still enforces compositor ownership");
  expect(report(providers, loop, shell.peer.get(), peers, native_report()).empty(),
         "current Shell reports native and XWayland clients");
  const auto scan = providers.windows();
  if (!expect(providers.has_windows() && scan.authoritative && scan.windows.size() == 2,
              "the bus exposes only its own authoritative reports")) return;
  providers.use_window_source(&providers);
  const auto native = scan.windows[0].window;
  expect(scan.windows[0].executable_path == platform::process_executable_path(::getpid()) &&
             scan.windows[0].active && scan.windows[0].width == 200,
         "same-UID PID and global logical geometry survive the adapter");
  expect(!providers.x11_window(native) && providers.x11_window(scan.windows[1].window) == 17,
         "opaque native identities are distinct from explicitly reported XIDs");
  const auto status = providers.window_provider();
  expect(status && status->state == runtime::DisplayProviderState::active,
         "accepted window provider has an active status");

  platform::OverlayFrame frame;
  frame.target = {10, 20, 200, 100};
  frame.panel.id = "p";
  const platform::GnomePanel a{"a", platform::build_overlay_display_list(frame)};
  const platform::GnomePanel b{"b", platform::build_overlay_display_list(frame)};
  expect(providers.show("a", frame) && providers.show("b", frame), "two panels are accumulated");
  providers.present();
  expect(pump(providers, loop, peers, [&] { return shell.presents == 1; }) &&
             shell.frame == *platform::build_gnome_overlay_frame(std::vector{a, b}),
         "one Present replaces the complete frame, rather than hiding sibling panels");
  providers.hide("a");
  providers.present();
  expect(pump(providers, loop, peers, [&] { return shell.presents == 2; }) &&
             shell.frame == *platform::build_gnome_overlay_frame(std::vector{b}),
         "hide preserves other live panels");

  // Frame bounds: a rejected panel degrades the overlay but leaves the
  // transport and accepted panels intact; freeing room recovers it.
  bool filled = true;
  for (int i = 0; i < 63; ++i) filled = providers.show("p" + std::to_string(i), frame) && filled;
  expect(filled, "64 panels fit the frame bounds");
  expect(!providers.show("over", frame), "a 65th panel is rejected");
  auto overlay_status = providers.overlay_provider();
  expect(overlay_status && overlay_status->state == runtime::DisplayProviderState::degraded &&
             overlay_status->reasons == std::vector<std::string>{"gnome_frame_too_large"} &&
             providers.overlay() != nullptr,
         "rejection is visible without disabling the transport");
  providers.hide("p0");
  expect(providers.show("over", frame), "freeing a slot lets the rejected panel recover");
  overlay_status = providers.overlay_provider();
  expect(overlay_status && overlay_status->state == runtime::DisplayProviderState::active,
         "recovery clears the bounds failure");
  providers.present();
  expect(pump(providers, loop, peers, [&] { return shell.presents == 3; }),
         "the recovered frame is presented");
  const std::vector<std::string> keep{"b"};
  providers.retain(keep);

  auto kwin = fixture::peer(platform::kwin_bus_name);
  auto all = peers; all.push_back(kwin.get());
  expect(pump(providers, loop, all, [&] { return providers.kwin_owner(); }),
         "late KWin owner beats a GNOME desktop hint");
  expect(!providers.has_windows() && providers.windows().windows.empty(),
         "a different compositor cannot reuse the old Shell report");
  kwin.reset();
  expect(pump(providers, loop, peers, [&] { return !providers.kwin_owner() && providers.overlay(); }),
         "owner loss recovers the still-current GNOME route");
  providers.present();
  expect(pump(providers, loop, peers, [&] {
    return shell.frame.empty(); // the transport uses Clear for an empty frame
  }), "old compositor panel identities never replay after a route replacement");

  providers.configure(false, "GNOME", false);
  expect(pump(providers, loop, peers, [&] { return !providers.connected(); }),
         "demand drop asynchronously drains cleanup then closes the bus");
  expect(!providers.next_deadline(), "no retry or reply timer survives bus quiescence");
  loop.set_deadline(std::nullopt);
  expect(!loop.poll_once(10).any(), "idle host has no busy ready descriptor");
}

#ifdef LAYERC_TEST_PIPEWIRE
struct DeniedPortal {
  fixture::Peer peer{fixture::peer(platform::portal_bus_name)};
  platform::BusSlot object;
  unsigned creates{};
  DeniedPortal() {
    static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("version", "u", property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("AvailableSourceTypes", "u", property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_PROPERTY("AvailableCursorModes", "u", property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("CreateSession", "a{sv}", "o", create, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
    object = fixture::method(peer.get(), platform::portal_path,
                            platform::portal_screencast_interface, vtable, this);
  }
  static int property(sd_bus *, const char *, const char *, const char *name,
                      sd_bus_message *reply, void *, sd_bus_error *) {
    const std::uint32_t value = std::string_view{name} == "version" ? 4 : 3;
    return sd_bus_message_append(reply, "u", value);
  }
  static int create(sd_bus_message *call, void *data, sd_bus_error *) {
    auto &self = *static_cast<DeniedPortal *>(data);
    ++self.creates;
    std::string token;
    if (sd_bus_message_enter_container(call, 'a', "{sv}") <= 0) return -EINVAL;
    while (sd_bus_message_enter_container(call, 'e', "sv") > 0) {
      const char *key{}, *value{};
      if (sd_bus_message_read(call, "s", &key) < 0 ||
          sd_bus_message_enter_container(call, 'v', "s") <= 0 ||
          sd_bus_message_read(call, "s", &value) < 0) return -EINVAL;
      if (std::string_view{key} == "handle_token") token = value;
      if (sd_bus_message_exit_container(call) < 0 ||
          sd_bus_message_exit_container(call) < 0) return -EINVAL;
    }
    const auto path = platform::portal_request_path(sd_bus_message_get_sender(call), token);
    const int result = sd_bus_reply_method_return(call, "o", path.c_str());
    sd_bus_message *signal{};
    if (sd_bus_message_new_signal(self.peer.get(), &signal, path.c_str(),
        "org.freedesktop.portal.Request", "Response") < 0) return -ENOMEM;
    (void)sd_bus_message_append(signal, "u", std::uint32_t{1});
    (void)sd_bus_message_open_container(signal, 'a', "{sv}");
    (void)sd_bus_message_close_container(signal);
    (void)sd_bus_send(self.peer.get(), signal, nullptr);
    sd_bus_message_unref(signal);
    return result;
  }
};

void consent_failure_is_sticky() {
  FakeGnome shell;
  DeniedPortal portal;
  const std::vector peers{shell.peer.get(), portal.peer.get()};
  platform::EventLoop loop;
  daemon::BusProviders providers{loop};
  providers.configure(true, "GNOME", false);
  expect(pump(providers, loop, peers, [&] { return providers.gnome_owner(); }),
         "capture host discovers the Shell owner");
  expect(report(providers, loop, shell.peer.get(), peers, native_report()).empty(), "native report accepted");
  const auto window = providers.windows().windows.front().window;
  providers.use_window_source(&providers);
  auto capture = providers.create_capture("fixture", {window}, core::CaptureQuality::low, 20);
  expect(!providers.capture_demanded() && portal.creates == 0, "allocating an owner does not ask consent");
  expect(capture->start(), "demand begins asynchronous consent");
  expect(pump(providers, loop, peers, [&] {
    return capture->poll(core::MonotonicClock::now()).result.diagnostic == "portal_denied";
  }), "portal denial crosses the native capture seam");
  expect(portal.creates == 1 && !capture->start(), "periodic reconcile never reprompts after denial");
  const auto status = providers.capture_provider();
  expect(status.state == runtime::DisplayProviderState::unavailable &&
             std::ranges::find(status.reasons, "portal_denied") != status.reasons.end(),
         "provider diagnostics retain the denial");
  expect(capture->resources().threads == 0 && capture->resources().capture_sessions == 0,
         "a denied session owns no PipeWire stream or background thread");
  providers.configure(true, "GNOME", true);
  expect(capture->start(), "a deliberate configuration change permits a new consent attempt");
  expect(pump(providers, loop, peers, [&] { return portal.creates == 2; }),
         "new demand is one explicit retry");
  capture.reset();
  expect(!providers.capture_demanded(), "dropping the capture owner removes demand");
  providers.stop();
  expect(pump(providers, loop, peers, [&] { return !providers.connected(); }) &&
             !providers.next_deadline(), "shutdown drains and returns to idle");
}

void capture_window_source_changes() {
  FakeGnome shell;
  DeniedPortal portal;
  const std::vector peers{shell.peer.get(), portal.peer.get()};
  platform::EventLoop loop;
  WindowSource first, second;
  daemon::BusProviders providers{loop};
  providers.use_window_source(&first);
  providers.configure(true, "GNOME", false);
  expect(pump(providers, loop, peers, [&] { return providers.gnome_owner(); }),
         "the capture-source fixture discovers the Shell owner");
  expect(!providers.has_windows(), "capture lookup does not require bus window reports");
  auto capture = providers.create_capture("source", {7}, core::CaptureQuality::low, 20);
  expect(capture->start(), "portal capture resolves a separately selected window source");
  providers.use_window_source(&first);
  expect(capture->poll(core::MonotonicClock::now()).result.diagnostic != "window_provider_changed",
         "retaining the same source does not invalidate capture");
  providers.use_window_source(&second);
  expect(capture->poll(core::MonotonicClock::now()).result.diagnostic == "window_provider_changed" &&
             !capture->start(),
         "changing source invalidates capture even when its window id still exists");
  providers.use_window_source(&first);
  expect(capture->poll(core::MonotonicClock::now()).result.diagnostic == "window_provider_changed",
         "returning to the original source does not clear the sticky fault");
  capture.reset();
  providers.stop();
  expect(pump(providers, loop, peers, [&] { return !providers.connected(); }),
         "the capture-source fixture drains shutdown");
}
#endif
} // namespace

#ifdef LAYERC_TEST_PIPEWIRE
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(reporting_overlay_and_idle),
                 LAYERC_TEST_CASE(consent_failure_is_sticky),
                 LAYERC_TEST_CASE(capture_window_source_changes))
#else
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(reporting_overlay_and_idle))
#endif
