// X11 connection, extension probes and EWMH tracking (V2.6.1). Runs against
// Xvfb: the test publishes its own EWMH window-manager fixture, so the tracker
// is verified without a real WM. Returns 77 (ctest skip) when no $DISPLAY or no
// server is reachable.
#include "x11_display.hpp"
#include "x11_error.hpp"
#include "x11_window.hpp"

#include "test_support.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using layerc::platform::X11Display;
using layerc::platform::X11WindowScan;
using layerc::platform::X11WindowSnapshot;
using layerc::platform::X11WindowTracker;


[[nodiscard]] std::string self_executable() {
  return layerc::platform::process_executable_path(::getpid());
}

[[nodiscard]] std::string basename_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void set_window_property(Display *display, ::Window window, const char *atom_name,
                         Atom type, int format, const void *data,
                         int count) {
  const Atom atom = XInternAtom(display, atom_name, False);
  XChangeProperty(display, window, atom, type, format, PropModeReplace,
                  static_cast<const unsigned char *>(data), count);
}

[[nodiscard]] X11Display open_display() {
  layerc::test::require_x11();
  std::string error;
  auto display = X11Display::open(error);
  if (!display) {
    std::fprintf(stderr, "skip: %s\n", error.c_str());
    layerc::test::skip();
  }
  return std::move(*display);
}

struct EwmhFixture {
  Display *raw;
  ::Window root;
  ::Window wm_window;
  ::Window client;

  explicit EwmhFixture(X11Display &display)
      : raw(reinterpret_cast<Display *>(display.raw())),
        root(DefaultRootWindow(raw)),
        wm_window(XCreateSimpleWindow(raw, root, 0, 0, 1, 1, 0, 0, 0)),
        client(XCreateSimpleWindow(raw, root, 10, 20, 300, 200, 0, 0, 0)) {
    XMapWindow(raw, client);
    XSync(raw, False);
    const unsigned long pid = static_cast<unsigned long>(::getpid());
    const char title[] = "Alpha Window";
    XClassHint hint{};
    hint.res_name = const_cast<char *>("alpha");
    hint.res_class = const_cast<char *>("AlphaApp");
    XSetClassHint(raw, client, &hint);
    const Atom utf8 = XInternAtom(raw, "UTF8_STRING", False);
    set_window_property(raw, client, "_NET_WM_NAME", utf8, 8, title,
                        static_cast<int>(std::strlen(title)));
    set_window_property(raw, client, "_NET_WM_PID", XA_CARDINAL, 32, &pid, 1);
    set_window_property(raw, root, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW, 32,
                        &wm_window, 1);
    set_window_property(raw, wm_window, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW,
                        32, &wm_window, 1);
    set_window_property(raw, root, "_NET_CLIENT_LIST", XA_WINDOW, 32, &client, 1);
    set_window_property(raw, root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 32, &client, 1);
    XFlush(raw);
  }

  ~EwmhFixture() {
    XDestroyWindow(raw, client);
    XDestroyWindow(raw, wm_window);
    for (const char *name : {"_NET_SUPPORTING_WM_CHECK", "_NET_CLIENT_LIST",
                             "_NET_ACTIVE_WINDOW"})
      XDeleteProperty(raw, root, XInternAtom(raw, name, False));
    XSync(raw, False);
  }
};

void probe_extensions() {
  using layerc::test::expect;
  auto display = open_display();
  const auto &probe = display.probe();
  expect(probe.connected, "probe reports the connection");
  expect(display.fd() >= 0, "connection exposes a pollable fd");
  expect(display.root() != 0, "root window id is published");
  expect(probe.xi2.present && probe.xi2.major >= 2, "XI2 >= 2.0 present");
  expect(probe.shape.present, "Shape present");
  expect(probe.randr.present, "RandR present");
  expect(display.drain_events() >= 0, "drain does not fault");
}

void error_traps() {
  using layerc::test::expect;
  auto display = open_display();
  auto *raw = reinterpret_cast<Display *>(display.raw());
  const ::Window root = DefaultRootWindow(raw);
  // Traps synchronize the guarded serial range, and a late BadWindow cannot
  // leak into the next operation.
  const ::Window destroyed =
      XCreateSimpleWindow(raw, root, 0, 0, 1, 1, 0, 0, 0);
  XDestroyWindow(raw, destroyed);
  XSync(raw, False);
  {
    layerc::platform::X11ErrorTrap trap{raw};
    XMapWindow(raw, destroyed);
    expect(!trap.ok(), "trap observes an asynchronous BadWindow");
  }
  {
    layerc::platform::X11ErrorTrap trap{raw};
    expect(trap.ok(), "next trap is not poisoned by a late error");
  }
}

void ewmh_scan_and_fallback() {
  using layerc::test::expect;
  auto display = open_display();
  EwmhFixture fixture(display);
  std::string reopen_error;
  auto tracker_display = X11Display::open(reopen_error);
  if (!expect(tracker_display.has_value(), "reopen after publishing EWMH"))
    return;
  expect(tracker_display->probe().ewmh,
                   "EWMH advertised once _NET_SUPPORTING_WM_CHECK is set");

  X11WindowTracker tracker(*tracker_display);
  const X11WindowScan scan = tracker.scan();
  expect(scan.ewmh, "scan uses the EWMH client list");
  expect(scan.warning.empty(), "no warning on the EWMH path");
  expect(scan.truncated == 0, "small scans are not truncated");

  const X11WindowSnapshot *found = nullptr;
  for (const auto &snapshot : scan.windows)
    if (snapshot.window == static_cast<std::uint64_t>(fixture.client))
      found = &snapshot;
  expect(found != nullptr, "fixture window appears in the EWMH scan");
  if (found != nullptr) {
    expect(found->title == "Alpha Window",
                     "title comes from _NET_WM_NAME");
    expect(found->class_name == "AlphaApp",
                     "class comes from WM_CLASS res_class");
    expect(found->instance_name == "alpha",
                     "instance comes from WM_CLASS res_name");
    expect(found->pid.has_value() &&
                         *found->pid == static_cast<std::uint32_t>(::getpid()),
                     "pid comes from _NET_WM_PID");
    expect(found->executable_name == basename_of(self_executable()),
                     "executable name resolves for a same-UID pid");
    expect(!found->executable_path.empty() &&
                         found->executable_path.front() == '/',
                     "executable path is absolute when resolvable");
    expect(found->x == 10 && found->y == 20,
                     "root-relative position");
    expect(found->width == 300 && found->height == 200, "geometry");
    expect(found->active, "active window flagged");
    expect(!found->fullscreen && !found->hidden,
                     "no _NET_WM_STATE flags on the fixture");
  }
  expect(scan.active.has_value() &&
             *scan.active == static_cast<std::uint64_t>(fixture.client),
         "_NET_ACTIVE_WINDOW published");

  // --- bounded fallback: drop the client list, keep the root children ------
  XDeleteProperty(fixture.raw, fixture.root,
                  XInternAtom(fixture.raw, "_NET_CLIENT_LIST", False));
  XFlush(fixture.raw);
  const X11WindowScan fallback = tracker.scan();
  expect(!fallback.ewmh, "empty client list falls back");
  expect(!fallback.warning.empty(), "fallback reports a reason");
  bool fallback_found = false;
  for (const auto &snapshot : fallback.windows)
    if (snapshot.window == static_cast<std::uint64_t>(fixture.client))
      fallback_found = true;
  expect(fallback_found,
                   "root-child fallback still finds the mapped window");

}

void proc_resolution() {
  using layerc::test::expect;
  // --- /proc resolution ---------------------------------------------------
  expect(layerc::platform::process_executable_path(
                       static_cast<std::uint32_t>(::getpid())) ==
                       self_executable(),
                   "same-UID pid resolves its executable");
  expect(
      layerc::platform::process_executable_path(4000000).empty(),
      "unknown pid resolves to nothing");
  const auto init_path = layerc::platform::process_executable_path(1);
  expect(init_path.empty() || init_path.front() == '/',
                   "another user's pid resolves to nothing or an absolute path");

}

void window_matching() {
  using layerc::test::expect;
  // --- matching (pure: needs no server) -----------------------------------
  X11WindowScan fake;
  X11WindowSnapshot first;
  first.window = 1;
  first.executable_name = "game.exe";
  first.executable_path = "/usr/bin/game.exe";
  first.class_name = "steam_app";
  first.title = "Some Game";
  X11WindowSnapshot second;
  second.window = 2;
  second.executable_name = "game.exe";
  second.class_name = "Unrelated";
  second.title = "Idle";
  second.active = true;
  fake.windows = {first, second};
  fake.active = 2;

  layerc::core::GameMatch by_name;
  by_name.executable_name = "GAME.EXE";
  expect(layerc::platform::match_window(fake, by_name) ==
                       &fake.windows[1],
                   "executable-name matching prefers the active window");

  layerc::core::GameMatch by_path;
  by_path.executable_path = "/usr/bin/other.bin";
  expect(layerc::platform::match_window(fake, by_path) == nullptr,
                   "exact path opt-in does not match a different binary");

  layerc::core::GameMatch by_class;
  by_class.window_class = "Steam";
  expect(layerc::platform::match_window(fake, by_class) ==
                       &fake.windows[0],
                   "class match is a substring fold");

  layerc::core::GameMatch by_title;
  by_title.window_title = "some game";
  expect(layerc::platform::match_window(fake, by_title) ==
                       &fake.windows[0],
                   "title match is a case-folded substring");

  layerc::core::GameMatch global_only;
  global_only.global = true;
  expect(layerc::platform::match_window(fake, global_only) ==
                       &fake.windows[1],
                   "global profiles follow the active window");

  layerc::core::GameMatch no_identity;
  expect(layerc::platform::match_window(fake, no_identity) == nullptr,
                   "a profile without identity fields never matches");
  X11WindowScan empty_scan;
  expect(layerc::platform::match_window(empty_scan, by_name) ==
                       nullptr,
                   "no windows means no match");

}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(probe_extensions),
                 LAYERC_TEST_CASE(error_traps),
                 LAYERC_TEST_CASE(ewmh_scan_and_fallback),
                 LAYERC_TEST_CASE(proc_resolution),
                 LAYERC_TEST_CASE(window_matching))
