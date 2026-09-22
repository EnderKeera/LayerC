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

using layerc::test::skip_code;

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

} // namespace

int run_linux_x11_tests() {
  using layerc::test::expect;
  bool passed = true;

  if (!layerc::test::x11_display_available()) {
    std::fprintf(stderr, "skip: DISPLAY is not set (run under Xvfb)\n");
    return skip_code;
  }

  std::string error;
  auto display = X11Display::open(error);
  if (!display.has_value()) {
    std::fprintf(stderr, "skip: %s\n", error.c_str());
    return skip_code;
  }

  // --- probes -------------------------------------------------------------
  const auto &probe = display->probe();
  passed &= expect(probe.connected, "probe reports the connection");
  passed &= expect(display->fd() >= 0, "connection exposes a pollable fd");
  passed &= expect(display->root() != 0, "root window id is published");
  passed &=
      expect(probe.xi2.present && probe.xi2.major >= 2, "XI2 >= 2.0 present");
  passed &= expect(probe.shape.present, "Shape present");
  passed &= expect(probe.randr.present, "RandR present");
  for (const auto &reason : probe.unavailable())
    std::printf("probe reason: %s\n", reason.c_str());
  passed &= expect(display->drain_events() >= 0, "drain does not fault");

  auto *raw = reinterpret_cast<Display *>(display->raw());
  const ::Window root = DefaultRootWindow(raw);
  const Atom utf8 = XInternAtom(raw, "UTF8_STRING", False);

  // Traps synchronize the guarded serial range, and a late BadWindow cannot
  // leak into the next operation.
  const ::Window destroyed =
      XCreateSimpleWindow(raw, root, 0, 0, 1, 1, 0, 0, 0);
  XDestroyWindow(raw, destroyed);
  XSync(raw, False);
  {
    layerc::platform::X11ErrorTrap trap{raw};
    XMapWindow(raw, destroyed);
    passed &= expect(!trap.ok(), "trap observes an asynchronous BadWindow");
  }
  {
    layerc::platform::X11ErrorTrap trap{raw};
    passed &= expect(trap.ok(), "next trap is not poisoned by a late error");
  }

  // --- EWMH fixture: publish a fake WM, then reopen so the probe sees it ---
  const ::Window wm_window = XCreateSimpleWindow(raw, root, 0, 0, 1, 1, 0, 0, 0);
  const ::Window client = XCreateSimpleWindow(raw, root, 10, 20, 300, 200, 0, 0, 0);
  XMapWindow(raw, client);
  XSync(raw, False);

  const unsigned long pid = static_cast<unsigned long>(::getpid());
  const char title[] = "Alpha Window";
  XClassHint hint{};
  hint.res_name = const_cast<char *>("alpha");
  hint.res_class = const_cast<char *>("AlphaApp");
  XSetClassHint(raw, client, &hint);
  set_window_property(raw, client, "_NET_WM_NAME", utf8, 8, title,
                      static_cast<int>(std::strlen(title)));
  set_window_property(raw, client, "_NET_WM_PID", XA_CARDINAL, 32, &pid, 1);
  set_window_property(raw, root, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW, 32,
                      &wm_window, 1);
  set_window_property(raw, wm_window, "_NET_SUPPORTING_WM_CHECK", XA_WINDOW, 32,
                      &wm_window, 1);
  set_window_property(raw, root, "_NET_CLIENT_LIST", XA_WINDOW, 32, &client, 1);
  set_window_property(raw, root, "_NET_ACTIVE_WINDOW", XA_WINDOW, 32, &client, 1);
  XFlush(raw);

  std::string reopen_error;
  auto tracker_display = X11Display::open(reopen_error);
  passed &= expect(tracker_display.has_value(), "reopen after publishing EWMH");
  if (!tracker_display.has_value()) return passed ? 0 : 1;
  passed &= expect(tracker_display->probe().ewmh,
                   "EWMH advertised once _NET_SUPPORTING_WM_CHECK is set");

  X11WindowTracker tracker(*tracker_display);
  const X11WindowScan scan = tracker.scan();
  passed &= expect(scan.ewmh, "scan uses the EWMH client list");
  passed &= expect(scan.warning.empty(), "no warning on the EWMH path");
  passed &= expect(scan.truncated == 0, "small scans are not truncated");

  const X11WindowSnapshot *found = nullptr;
  for (const auto &snapshot : scan.windows)
    if (snapshot.window == static_cast<std::uint64_t>(client)) found = &snapshot;
  passed &= expect(found != nullptr, "fixture window appears in the EWMH scan");
  if (found != nullptr) {
    passed &= expect(found->title == "Alpha Window",
                     "title comes from _NET_WM_NAME");
    passed &= expect(found->class_name == "AlphaApp",
                     "class comes from WM_CLASS res_class");
    passed &= expect(found->instance_name == "alpha",
                     "instance comes from WM_CLASS res_name");
    passed &= expect(found->pid.has_value() &&
                         *found->pid == static_cast<std::uint32_t>(::getpid()),
                     "pid comes from _NET_WM_PID");
    passed &= expect(found->executable_name == basename_of(self_executable()),
                     "executable name resolves for a same-UID pid");
    passed &= expect(!found->executable_path.empty() &&
                         found->executable_path.front() == '/',
                     "executable path is absolute when resolvable");
    passed &= expect(found->x == 10 && found->y == 20,
                     "root-relative position");
    passed &= expect(found->width == 300 && found->height == 200, "geometry");
    passed &= expect(found->active, "active window flagged");
    passed &= expect(!found->fullscreen && !found->hidden,
                     "no _NET_WM_STATE flags on the fixture");
  }
  passed &= expect(scan.active.has_value() &&
                       *scan.active == static_cast<std::uint64_t>(client),
                   "_NET_ACTIVE_WINDOW published");

  // --- bounded fallback: drop the client list, keep the root children ------
  XDeleteProperty(raw, root, XInternAtom(raw, "_NET_CLIENT_LIST", False));
  XFlush(raw);
  const X11WindowScan fallback = tracker.scan();
  passed &= expect(!fallback.ewmh, "empty client list falls back");
  passed &= expect(!fallback.warning.empty(), "fallback reports a reason");
  bool fallback_found = false;
  for (const auto &snapshot : fallback.windows)
    if (snapshot.window == static_cast<std::uint64_t>(client))
      fallback_found = true;
  passed &= expect(fallback_found,
                   "root-child fallback still finds the mapped window");

  // --- /proc resolution ---------------------------------------------------
  passed &= expect(layerc::platform::process_executable_path(
                       static_cast<std::uint32_t>(::getpid())) ==
                       self_executable(),
                   "same-UID pid resolves its executable");
  passed &= expect(
      layerc::platform::process_executable_path(4000000).empty(),
      "unknown pid resolves to nothing");
  const auto init_path = layerc::platform::process_executable_path(1);
  passed &= expect(init_path.empty() || init_path.front() == '/',
                   "another user's pid resolves to nothing or an absolute path");

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
  passed &= expect(layerc::platform::match_window(fake, by_name) ==
                       &fake.windows[1],
                   "executable-name matching prefers the active window");

  layerc::core::GameMatch by_path;
  by_path.executable_path = "/usr/bin/other.bin";
  passed &= expect(layerc::platform::match_window(fake, by_path) == nullptr,
                   "exact path opt-in does not match a different binary");

  layerc::core::GameMatch by_class;
  by_class.window_class = "Steam";
  passed &= expect(layerc::platform::match_window(fake, by_class) ==
                       &fake.windows[0],
                   "class match is a substring fold");

  layerc::core::GameMatch by_title;
  by_title.window_title = "some game";
  passed &= expect(layerc::platform::match_window(fake, by_title) ==
                       &fake.windows[0],
                   "title match is a case-folded substring");

  layerc::core::GameMatch global_only;
  global_only.global = true;
  passed &= expect(layerc::platform::match_window(fake, global_only) ==
                       &fake.windows[1],
                   "global profiles follow the active window");

  layerc::core::GameMatch no_identity;
  passed &= expect(layerc::platform::match_window(fake, no_identity) == nullptr,
                   "a profile without identity fields never matches");
  X11WindowScan empty_scan;
  passed &= expect(layerc::platform::match_window(empty_scan, by_name) ==
                       nullptr,
                   "no windows means no match");

  XDestroyWindow(raw, client);
  XDestroyWindow(raw, wm_window);
  XDeleteProperty(raw, root, XInternAtom(raw, "_NET_SUPPORTING_WM_CHECK", False));
  XDeleteProperty(raw, root, XInternAtom(raw, "_NET_ACTIVE_WINDOW", False));
  XFlush(raw);

  std::printf("x11 tests: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}

int main() {
  return layerc::test::run({{"linux_x11_tests", [] {
                              const int result = run_linux_x11_tests();
                              if (result == skip_code) layerc::test::skip();
                              layerc::test::expect(
                                  result == 0,
                                  "the X11 platform program should succeed");
                            }}});
}
