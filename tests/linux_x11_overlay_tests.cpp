// Click-through Shape overlays and monitor scale (V2.6.1). Runs against
// Xvfb: an overlay window is created, its ShapeInput region must stay empty,
// and the painted pixels must match the panel's colors. Returns 77 (ctest
// skip) when no $DISPLAY is reachable.
//
// The geometry and scale rules are pure and are checked before the server is
// touched, so a headless environment still verifies the math.
#include "input_service.hpp"
#include "overlay_service.hpp"
#include "x11_provider.hpp"
#include "x11_display.hpp"
#include "x11_monitor.hpp"
#include "x11_overlay.hpp"
#include "x11_window.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#if defined(LAYERC_TEST_XTEST)
#include <X11/extensions/XTest.h>
#endif

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using layerc::core::GameProfile;
using layerc::platform::StoredProfile;
using layerc::platform::X11Display;
using layerc::platform::X11Monitor;
using layerc::platform::X11MonitorScan;
using layerc::platform::X11OverlayBar;
using layerc::platform::X11OverlayFrame;
using layerc::platform::X11OverlayRect;
using layerc::platform::X11WindowScan;

using layerc::test::skip_code;

[[nodiscard]] std::string self_executable() {
  return layerc::platform::process_executable_path(::getpid());
}

[[nodiscard]] std::string basename_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] X11OverlayFrame bar_frame(const int dpi, const int bar_count,
                                        const X11OverlayRect &target) {
  X11OverlayFrame frame;
  frame.target = target;
  frame.dpi = dpi;
  frame.panel.id = "main";
  frame.panel.style = layerc::core::PanelStyle::bar;
  frame.panel.bar.width = 260;
  frame.panel.bar.height = 34;
  frame.panel.anchor_x = 0.5;
  frame.panel.anchor_y = 0.5;
  for (int index = 0; index < bar_count; ++index)
    frame.bars.push_back(X11OverlayBar{"Dash", 1.0, true});
  return frame;
}

[[nodiscard]] X11MonitorScan two_monitors() {
  X11MonitorScan scan;
  scan.available = true;
  X11Monitor left;
  left.name = "left";
  left.width = 1920;
  left.height = 1080;
  left.dpi = 96;
  left.primary = true;
  X11Monitor right;
  right.name = "right";
  right.x = 1920;
  right.width = 3840;
  right.height = 2160;
  right.dpi = 192;
  scan.monitors.push_back(left);
  scan.monitors.push_back(right);
  return scan;
}

void check_scale_rules(bool &passed) {
  using layerc::test::expect;
  using layerc::platform::baseline_dpi;
  using layerc::platform::dpi_from_geometry;
  using layerc::platform::max_dpi;
  using layerc::platform::monitor_dpi_for_rect;
  using layerc::platform::scaled_pixels;

  passed &= expect(dpi_from_geometry(1920, 1080, 0, 0) == baseline_dpi,
                   "unknown physical size falls back to the 96 dpi baseline");
  passed &= expect(dpi_from_geometry(1920, 1080, 531, 299) == 92,
                   "dpi is the mean of both axes");
  passed &= expect(dpi_from_geometry(3840, 2160, 40, 25) == max_dpi,
                   "absurd physical size is clamped");
  passed &= expect(scaled_pixels(260, 96) == 260, "96 dpi is a no-op");
  passed &= expect(scaled_pixels(260, 192) == 520, "192 dpi doubles geometry");
  passed &= expect(scaled_pixels(34, 100) == 35, "scale rounds to nearest");

  const auto scan = two_monitors();
  passed &= expect(monitor_dpi_for_rect(scan, 100, 100, 200, 100) == 96,
                   "a rect on the left monitor uses its scale");
  passed &= expect(monitor_dpi_for_rect(scan, 2000, 100, 200, 100) == 192,
                   "a rect on the right monitor uses its scale");
  passed &= expect(monitor_dpi_for_rect(scan, 1900, 100, 200, 100) == 192,
                   "the largest overlap wins");
  passed &= expect(monitor_dpi_for_rect(scan, 9000, 9000, 10, 10) == 96,
                   "a rect on no monitor falls back to the primary");
  passed &= expect(monitor_dpi_for_rect(X11MonitorScan{}, 0, 0, 10, 10) ==
                       baseline_dpi,
                   "an empty scan falls back to the baseline");
}

void check_geometry_rules(bool &passed) {
  using layerc::test::expect;
  using layerc::platform::panel_rect;

  const X11OverlayRect target{100, 100, 800, 600};
  const auto single = panel_rect(bar_frame(96, 1, target));
  passed &= expect(single.width == 272, "one bar panel is 260 + 2x padding");
  passed &= expect(single.height == 46, "one bar panel is 34 + 2x padding");
  passed &= expect(single.x == 364 && single.y == 377,
                   "a centred panel is anchored at the target's middle");

  const auto two = panel_rect(bar_frame(96, 2, target));
  passed &= expect(two.height == 86, "a second bar adds height + spacing");

  const auto scaled = panel_rect(bar_frame(192, 2, target));
  passed &= expect(scaled.width == 544 && scaled.height == 172,
                   "192 dpi doubles the panel and its padding");
  passed &= expect(scaled.x == 228 && scaled.y == 314,
                   "the doubled panel stays centred");

  X11OverlayFrame card{};
  card.target = target;
  card.dpi = 96;
  card.panel.id = "cards";
  card.panel.style = layerc::core::PanelStyle::card;
  card.panel.layout = layerc::core::PanelLayout::grid;
  card.panel.anchor_x = 0.5;
  card.panel.anchor_y = 0.5;
  for (int index = 0; index < 4; ++index)
    card.bars.push_back(X11OverlayBar{"Dash", 0.5, true});
  const auto grid = panel_rect(card);
  std::printf("card grid %dx%d\n", grid.width, grid.height);
  passed &= expect(grid.width == 174 && grid.height == 162,
                   "a 2x2 card grid is 2 items + padding + spacing");

  card.panel.layout = layerc::core::PanelLayout::column;
  card.bars.resize(3);
  const auto column = panel_rect(card);
  std::printf("card column %dx%d\n", column.width, column.height);
  passed &=
      expect(column.width == 90 && column.height == 240,
             "a column of three cards is one item wide and three items tall");

  X11OverlayFrame anchored = bar_frame(96, 1, target);
  anchored.panel.anchor_x = 0.0;
  anchored.panel.anchor_y = 1.0;
  const auto corner = panel_rect(anchored);
  passed &= expect(corner.x == -36 && corner.y == 677,
                   "anchors offset the panel by half its size");
}

void check_missing_shape_path(bool &passed) {
  using layerc::platform::overlay_unavailable;
  using layerc::test::expect;

  layerc::platform::X11Probe ready;
  ready.connected = true;
  ready.shape.present = true;
  passed &= expect(overlay_unavailable(ready).empty(),
                   "a Shape server can host an overlay");

  layerc::platform::X11Probe no_shape = ready;
  no_shape.shape.present = false;
  const auto reasons = overlay_unavailable(no_shape);
  passed &= expect(reasons.size() == 1, "missing Shape disables the overlay");
  if (!reasons.empty())
    std::printf("overlay reason: %s\n", reasons.front().c_str());

  layerc::platform::X11Probe disconnected;
  passed &= expect(!overlay_unavailable(disconnected).empty(),
                   "no display disables the overlay");
  X11Display display;
  layerc::daemon::X11OverlayBackend backend(display);
  passed &= expect(backend.unavailable() == std::vector<std::string>{
                       "overlay: no X display for the overlay service"},
                   "an invalid display preserves the overlay service reason");
  passed &= expect(!backend.show("missing", bar_frame(96, 1, {0, 0, 400, 300})) &&
                       backend.windows().empty(),
                   "an invalid display cannot create an overlay window");
}

[[nodiscard]] std::string fixture_profile(std::string_view executable_name) {
  std::string json =
      R"json({"schemaVersion":4,"modules":[],"hidBindings":[],"cooldownGroups":[],"id":"overlay-fixture","name":"Overlay Fixture","game":{"executableName":"NAME"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":60000,"recovery":"single"}],"panels":[{"id":"main","name":"Main","style":"bar","bar":{"width":260,"height":34,"fillColor":"#50bac8","backgroundColor":"#272d35","borderColor":"#50bac8","textColor":"#e4e8ec","showText":true,"hideWhenReady":true},"anchorX":0.5,"anchorY":0.5,"cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":18}]}]})json";
  const auto at = json.find("NAME");
  json.replace(at, 4, executable_name);
  return json;
}

[[nodiscard]] layerc::core::AutomationProfile
overlay_automation(std::string profile_id) {
  using layerc::core::AutomationActionKind;
  using layerc::core::AutomationEventKind;

  layerc::core::AutomationAction hide;
  hide.kind = AutomationActionKind::set_overlay_override;
  hide.target = "main";
  hide.name = "hide";
  hide.property = "visible";
  hide.value.emplace();
  hide.value->literal = layerc::core::AutomationValue(false);

  layerc::core::AutomationRule pressed;
  pressed.id = "pressed";
  pressed.event = AutomationEventKind::input_pressed;
  pressed.source_filter = "keyboard:18";
  pressed.actions = {hide};

  layerc::core::AutomationAction clear;
  clear.kind = AutomationActionKind::clear_overlay_override;
  clear.target = "hide";

  layerc::core::AutomationRule released;
  released.id = "released";
  released.event = AutomationEventKind::input_released;
  released.source_filter = "keyboard:18";
  released.actions = {clear};

  auto show = hide;
  show.name = "show";
  show.value->literal = layerc::core::AutomationValue(true);
  auto show_pressed = pressed;
  show_pressed.id = "show-pressed";
  show_pressed.source_filter = "keyboard:70";
  show_pressed.actions = {show};
  auto clear_show = clear;
  clear_show.target = "show";
  auto show_released = released;
  show_released.id = "show-released";
  show_released.source_filter = "keyboard:70";
  show_released.actions = {clear_show};

  auto text = hide;
  text.name = "text";
  text.property = "text";
  text.value->literal = layerc::core::AutomationValue(std::string{"OVERRIDE"});
  auto text_pressed = pressed;
  text_pressed.id = "text-pressed";
  text_pressed.source_filter = "keyboard:71";
  text_pressed.actions = {text};
  auto clear_text = clear;
  clear_text.target = "text";
  auto text_released = released;
  text_released.id = "text-released";
  text_released.source_filter = "keyboard:71";
  text_released.actions = {clear_text};

  layerc::core::AutomationDefinition automation;
  automation.id = "overlay";
  automation.name = "Overlay";
  automation.rules = {pressed, released, show_pressed, show_released,
                      text_pressed, text_released};

  layerc::core::AutomationProfile profile;
  profile.profile_id = std::move(profile_id);
  profile.automations = {automation};
  return profile;
}

[[nodiscard]] bool same_pixels(XImage *left, XImage *right) {
  if (left == nullptr || right == nullptr || left->width != right->width ||
      left->height != right->height)
    return false;
  for (int y = 0; y < left->height; ++y)
    for (int x = 0; x < left->width; ++x)
      if (XGetPixel(left, x, y) != XGetPixel(right, x, y)) return false;
  return true;
}

[[nodiscard]] unsigned long alloc_pixel(Display *raw, const unsigned int rgb) {
  XColor color{};
  color.red = static_cast<unsigned short>(((rgb >> 16U) & 0xFFU) * 257);
  color.green = static_cast<unsigned short>(((rgb >> 8U) & 0xFFU) * 257);
  color.blue = static_cast<unsigned short>((rgb & 0xFFU) * 257);
  color.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(raw, DefaultColormap(raw, DefaultScreen(raw)), &color) == 0)
    return 0;
  return color.pixel;
}

} // namespace

int run_linux_x11_overlay_tests() {
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
  const bool shape_present = display->probe().shape.present;
  passed &= expect(display->probe().randr.present, "RandR present");
  passed &=
      expect(overlay_unavailable(display->probe()).empty() == shape_present,
             "the live probe agrees with the Shape requirement");
  if (!shape_present) {
    // The server is the one environment that can prove the disable path.
    layerc::daemon::OverlayService disabled;
    layerc::daemon::X11OverlayBackend disabled_backend(*display);
    disabled.use_backend(&disabled_backend);
    std::string create_error;
    auto window =
        layerc::platform::X11OverlayWindow::create(*display, create_error);
    passed &= expect(!window.has_value(), "no overlay window without Shape");
    passed &= expect(disabled.unavailable() ==
                         overlay_unavailable(display->probe()),
                     "the service publishes the Shape reason");
    return passed ? 0 : 1;
  }

  // --- monitors ----------------------------------------------------------
  const auto monitors = layerc::platform::scan_monitors(*display);
  passed &= expect(monitors.available, "the monitor scan used RandR");
  passed &= expect(monitors.warning.empty(), "the monitor scan has no warning");
  passed &= expect(!monitors.monitors.empty(), "at least one monitor");
  for (const auto &monitor : monitors.monitors)
    std::printf("monitor %s %dx%d+%d+%d dpi=%d primary=%d\n",
                monitor.name.c_str(), monitor.width, monitor.height, monitor.x,
                monitor.y, monitor.dpi, monitor.primary ? 1 : 0);
  if (!monitors.monitors.empty()) {
    const auto &first = monitors.monitors.front();
    passed &= expect(first.width > 0 && first.height > 0,
                     "the monitor has a usable geometry");
    passed &= expect(first.dpi >= layerc::platform::min_dpi &&
                         first.dpi <= layerc::platform::max_dpi,
                     "the derived dpi stays inside the clamp");
  }

  // --- fixture window the profile matches --------------------------------
  auto *raw = reinterpret_cast<Display *>(display->raw());
  const ::Window root = DefaultRootWindow(raw);
  const ::Window fixture =
      XCreateSimpleWindow(raw, root, 40, 60, 400, 300, 0, 0, 0);
  XMapWindow(raw, fixture);
  const char title[] = "Overlay Fixture";
  const Atom utf8 = XInternAtom(raw, "UTF8_STRING", False);
  const unsigned long pid = static_cast<unsigned long>(::getpid());
  XChangeProperty(raw, fixture, XInternAtom(raw, "_NET_WM_NAME", False), utf8,
                  8, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(title),
                  static_cast<int>(std::strlen(title)));
  XChangeProperty(raw, fixture, XInternAtom(raw, "_NET_WM_PID", False),
                  XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&pid), 1);
  XChangeProperty(raw, root, XInternAtom(raw, "_NET_ACTIVE_WINDOW", False),
                  XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&fixture), 1);
  XSync(raw, False);

  const auto parsed = layerc::core::parse_profile_json(
      fixture_profile(basename_of(self_executable())));
  const auto *profile = std::get_if<GameProfile>(&parsed);
  if (profile == nullptr)
    std::fprintf(stderr, "profile error: %s: %s\n",
                 std::get<layerc::core::ProfileError>(parsed).path.c_str(),
                 std::get<layerc::core::ProfileError>(parsed).message.c_str());
  passed &= expect(profile != nullptr, "the fixture profile parses");
  if (profile == nullptr) return passed ? 0 : 1;

  // --- service: session -> overlay window --------------------------------
  layerc::daemon::X11Provider provider(std::move(*display));
  layerc::daemon::SessionRegistry input_sessions;
  layerc::daemon::InputService input(input_sessions);
  layerc::daemon::OverlayService overlay;
  layerc::daemon::PassiveInputSource *const overlay_sources[]{&provider};
  input.use_sources(overlay_sources);
  layerc::daemon::X11OverlayBackend overlay_backend(provider.display());
  overlay.use_backend(&overlay_backend);
  const X11WindowScan scan = provider.windows();
  layerc::test::TempDirectory temp{"layerc-x11-overlay"};
  const StoredProfile profiles[] = {
      {.path = temp.path() / "profile.json", .profile = *profile}};
  input_sessions.configure(profiles);
  input_sessions.refresh(scan);
  passed &= expect(input_sessions.session_count() == 1, "the fixture window matches");

  auto background_scan = scan;
  background_scan.active.reset();
  for (auto &snapshot : background_scan.windows) snapshot.active = false;
  input_sessions.refresh(background_scan);
  passed &= expect(input_sessions.session_count() == 1 &&
                       !input_sessions.sessions().front().active,
                   "a background profile keeps an inactive session");
  overlay.refresh(input_sessions.sessions(), background_scan, monitors);
  passed &= expect(overlay.resources().objects == 0,
                   "an inactive session creates no overlay target");

  input_sessions.refresh(scan);
  overlay.refresh(input_sessions.sessions(), scan, monitors);
  const auto demand = layerc::runtime::calculate_capability_demand(
      input_sessions.session_requests(), false);
  passed &= expect(demand.required(layerc::runtime::Capability::overlay),
                   "an active session demands the overlay");
  layerc::runtime::RuntimeServiceRegistry services{};
  services[static_cast<std::size_t>(layerc::runtime::Capability::input)] = &input;
  services[static_cast<std::size_t>(layerc::runtime::Capability::overlay)] =
      &overlay;
  layerc::runtime::DaemonKernel kernel(services);
  passed &= expect(kernel.reconcile(demand),
                   "the kernel activates input and overlay together");
  passed &= expect(kernel.services()
                       .find(layerc::runtime::Capability::input) != nullptr,
                   "the input service survived the overlay activation");
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : input_sessions.sessions())
    (void)session.automation.cooldowns().trigger("dash", now);
  overlay.render(input_sessions.sessions(), now);
  passed &= expect(overlay.window_count() == 1, "one panel -> one window");
  passed &= expect(overlay.unavailable().empty(),
                   "the overlay has no unavailability reason");
  const auto windows = overlay_backend.windows();
  passed &= expect(windows.size() == 1, "the overlay publishes its windows");
  if (windows.empty()) return passed ? 0 : 1;
  const auto overlay_window = static_cast<::Window>(windows.front());

  {
    layerc::daemon::X11OverlayBackend degraded_backend(provider.display());
    layerc::daemon::OverlayService degraded_overlay;
    degraded_overlay.use_backend(&degraded_backend);
    degraded_overlay.refresh(input_sessions.sessions(), scan, monitors);
    auto &probe = const_cast<layerc::platform::X11Probe &>(provider.display().probe());
    const bool shape_present = probe.shape.present;
    probe.shape.present = false;
    const auto reconciled = degraded_overlay.reconcile(demand);
    passed &= expect(reconciled.status ==
                         layerc::runtime::ServiceReconcileStatus::degraded,
                     "missing Shape degrades a demanded overlay service");
    degraded_overlay.render(input_sessions.sessions(), now);
    passed &= expect(degraded_backend.windows().empty() &&
                         degraded_overlay.window_count() == 0,
                     "a reconciled degraded overlay creates no X11 window");
    overlay.render(input_sessions.sessions(), now);
    passed &= expect(overlay.window_count() == 0,
                     "missing Shape also prevents updating an existing overlay");
    probe.shape.present = shape_present;
    degraded_overlay.quiesce();
  }
  overlay.render(input_sessions.sessions(), now);

  // --- click-through: the input region is empty --------------------------
  int count = -1;
  int order = -1;
  XRectangle *rectangles =
      XShapeGetRectangles(raw, overlay_window, ShapeInput, &count, &order);
  if (rectangles != nullptr) XFree(rectangles);
  passed &= expect(count == 0, "the overlay's ShapeInput region is empty");
  const ::Window plain = XCreateSimpleWindow(raw, root, 0, 0, 8, 8, 0, 0, 0);
  int plain_count = -1;
  int plain_order = -1;
  rectangles =
      XShapeGetRectangles(raw, plain, ShapeInput, &plain_count, &plain_order);
  if (rectangles != nullptr) XFree(rectangles);
  passed &= expect(plain_count != 0,
                   "an unshaped window still takes input (query is live)");

  // --- geometry + paint --------------------------------------------------
  overlay.render(input_sessions.sessions(), now);
  passed &= expect(overlay.next_render_deadline(now).has_value(),
                   "a mapped overlay arms the repaint deadline");

  XWindowAttributes attributes{};
  XGetWindowAttributes(raw, overlay_window, &attributes);
  const auto expected = panel_rect(bar_frame(
      layerc::platform::monitor_dpi_for_rect(monitors, 40, 60, 400, 300), 1,
      X11OverlayRect{40, 60, 400, 300}));
  const bool geometry_matches =
      attributes.x == expected.x && attributes.y == expected.y &&
      attributes.width == expected.width &&
      attributes.height == expected.height && attributes.override_redirect;
  std::printf("overlay geometry %dx%d+%d+%d (expected %dx%d+%d+%d)\n",
              attributes.width, attributes.height, attributes.x, attributes.y,
              expected.width, expected.height, expected.x, expected.y);
  passed &= expect(geometry_matches,
                   "the server reports the panel rect, override-redirected");
  passed &= expect(attributes.map_state == IsViewable, "the overlay is mapped");
#if defined(LAYERC_TEST_XTEST)
  // XTest is linked only into this executable. The server routes a real
  // pointer press through the visible panel to the target window.
  XSelectInput(raw, fixture, ButtonPressMask);
  XSync(raw, False);
  int xtest_event_base{};
  int xtest_error_base{};
  int xtest_major{};
  int xtest_minor{};
  const bool xtest = XTestQueryExtension(raw, &xtest_event_base,
                                        &xtest_error_base, &xtest_major,
                                        &xtest_minor) != False;
  passed &= expect(xtest, "the X server exposes test-only XTest input");
  if (xtest) {
    passed &= expect(XTestFakeMotionEvent(raw, -1, 240, 210, CurrentTime),
                     "the test pointer moves over the panel");
    passed &= expect(XTestFakeButtonEvent(raw, 1, True, CurrentTime),
                     "the test pointer presses over the panel");
    passed &= expect(XTestFakeButtonEvent(raw, 1, False, CurrentTime),
                     "the test pointer releases over the panel");
    XSync(raw, False);
    XEvent click{};
    passed &= expect(XCheckTypedWindowEvent(raw, fixture, ButtonPress, &click),
                     "the target receives the press through the overlay");
  }
#endif

  // Halfway through the cooldown the fill covers half the track.
  overlay.render(input_sessions.sessions(), now + std::chrono::seconds{30});
  const unsigned long fill = alloc_pixel(raw, 0x50BAC8);
  const unsigned long background = alloc_pixel(raw, 0x272D35);
  XImage *image = XGetImage(raw, overlay_window, 0, 0,
                            static_cast<unsigned int>(expected.width),
                            static_cast<unsigned int>(expected.height),
                            AllPlanes, ZPixmap);
  passed &= expect(image != nullptr, "the overlay's pixels can be read back");
  if (image != nullptr) {
    const int middle = expected.height / 2;
    const auto left = XGetPixel(image, 8, middle);
    const auto right = XGetPixel(image, expected.width - 12, middle);
    std::printf("pixels left=0x%lx right=0x%lx (fill=0x%lx bg=0x%lx)\n", left,
                right, fill, background);
    passed &= expect(left == fill, "the running part of the bar is filled");
    passed &= expect(right == background,
                     "the elapsed part of the bar shows the track");
    XDestroyImage(image);
  }

  // --- hide when everything is ready -------------------------------------
  const auto ready = now + std::chrono::seconds{61};
  overlay.render(input_sessions.sessions(), ready);
  passed &= expect(!overlay.next_render_deadline(ready).has_value(),
                   "a hidden overlay disarms the repaint deadline");
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state != IsViewable,
                   "a ready bar panel is hidden");

  // A true visibility override keeps a ready bar mapped. A text override
  // changes its painted label and clearing it restores the normal label.
  auto &session = input_sessions.sessions().front();
  // start() reports whether session_started changed state; this fixture has
  // no session rule, so only the later input-driven overrides matter.
  (void)session.automation.start(
      overlay_automation(session.automation.profile().id), {}, ready);
  const layerc::core::InputEvent show_press{
      {layerc::core::InputDevice::keyboard, 70, 0}, true};
  const layerc::core::InputEvent show_release{
      {layerc::core::InputDevice::keyboard, 70, 0}, false};
  const layerc::core::InputEvent text_press{
      {layerc::core::InputDevice::keyboard, 71, 0}, true};
  const layerc::core::InputEvent text_release{
      {layerc::core::InputDevice::keyboard, 71, 0}, false};
  (void)session.feed(show_press, ready);
  overlay.render(input_sessions.sessions(), ready);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state == IsViewable &&
                       overlay.next_render_deadline(ready).has_value(),
                   "true visibility override shows a ready bar");
  XImage *normal_label = attributes.map_state == IsViewable
                             ? XGetImage(raw, overlay_window, 0, 0,
                                         static_cast<unsigned int>(expected.width),
                                         static_cast<unsigned int>(expected.height),
                                         AllPlanes, ZPixmap)
                             : nullptr;
  (void)session.feed(text_press, ready);
  overlay.render(input_sessions.sessions(), ready);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  XImage *custom_label = attributes.map_state == IsViewable
                             ? XGetImage(raw, overlay_window, 0, 0,
                                         static_cast<unsigned int>(expected.width),
                                         static_cast<unsigned int>(expected.height),
                                         AllPlanes, ZPixmap)
                             : nullptr;
  passed &= expect(normal_label != nullptr && custom_label != nullptr &&
                       !same_pixels(normal_label, custom_label),
                   "the text override changes painted label pixels");
  (void)session.feed(text_release, ready);
  overlay.render(input_sessions.sessions(), ready);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  XImage *restored_label = attributes.map_state == IsViewable
                               ? XGetImage(raw, overlay_window, 0, 0,
                                           static_cast<unsigned int>(expected.width),
                                           static_cast<unsigned int>(expected.height),
                                           AllPlanes, ZPixmap)
                               : nullptr;
  passed &= expect(same_pixels(normal_label, restored_label),
                   "clearing text restores the cooldown label");
  if (normal_label != nullptr) XDestroyImage(normal_label);
  if (custom_label != nullptr) XDestroyImage(custom_label);
  if (restored_label != nullptr) XDestroyImage(restored_label);
  (void)session.feed(show_release, ready);
  overlay.render(input_sessions.sessions(), ready);
  passed &= expect(!overlay.next_render_deadline(ready).has_value(),
                   "clearing true visibility restores hideWhenReady");

  // A new trigger brings it back: cooldown clocks only move forward.
  for (auto &session : input_sessions.sessions())
    (void)session.automation.cooldowns().trigger("dash", ready);
  const auto running = ready + std::chrono::seconds{10};
  overlay.render(input_sessions.sessions(), running);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state == IsViewable,
                   "a running cooldown shows the panel again");

  // Visibility automation hides the winning panel, then clearing the layer
  // rebuilds the frame from the profile's visible base state.
  const layerc::core::InputEvent press_e{
      {layerc::core::InputDevice::keyboard, 18, 0}, true};
  const layerc::core::InputEvent release_e{
      {layerc::core::InputDevice::keyboard, 18, 0}, false};
  (void)session.feed(press_e, running);
  overlay.render(input_sessions.sessions(), running);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state != IsViewable,
                   "a visibility override hides the rendered panel");

  (void)session.feed(release_e, running);
  overlay.render(input_sessions.sessions(), running);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state == IsViewable,
                   "clearing the override restores the base panel visibility");

  return passed ? 0 : 1;
}

void update_failure_recovery() {
  using layerc::test::expect;
  layerc::test::require_x11();
  std::string error;
  auto display = X11Display::open(error);
  if (!expect(display.has_value(), "open update-failure display")) return;
  layerc::daemon::X11OverlayBackend backend(*display);
  auto frame = bar_frame(96, 1, {0, 0, 400, 300});
  if (!expect(backend.show("faulted", frame), "create update-failure fixture")) return;
  const auto windows = backend.windows();
  if (!expect(windows.size() == 1, "one owned failure fixture exists")) return;
  XDestroyWindow(display->raw(), static_cast<::Window>(windows.front()));
  XSync(display->raw(), False);
  expect(!backend.show("faulted", frame), "a destroyed overlay fails update");
  expect(!backend.unavailable().empty(), "update failure has a visible reason");
  expect(backend.windows().empty(), "failed overlay is destroyed fail-closed");
  expect(backend.show("healthy", frame), "unrelated healthy overlay shows");
  expect(!backend.unavailable().empty(), "healthy key cannot mask another key's failure");
  expect(backend.show("faulted", frame), "failed overlay is recreated");
  expect(backend.unavailable().empty(), "successful recreation clears its fault");
  backend.retain({});

  if (!expect(backend.show("faulted", frame), "fault fixture can be recreated for hide")) return;
  const auto hidden_windows = backend.windows();
  if (!expect(hidden_windows.size() == 1, "hide fixture owns one window")) return;
  XDestroyWindow(display->raw(), static_cast<::Window>(hidden_windows.front()));
  XSync(display->raw(), False);
  expect(!backend.show("faulted", frame), "hide fixture update fails");
  backend.hide("faulted");
  expect(backend.unavailable().empty(), "intentional hide clears the matching fault");

  if (!expect(backend.show("healthy", frame), "healthy retained fixture shows")) return;
  const auto healthy_windows = backend.windows();
  if (!expect(healthy_windows.size() == 1, "one healthy retained window exists")) return;
  if (!expect(backend.show("faulted", frame), "faulted retained fixture shows")) return;
  const auto both_windows = backend.windows();
  ::Window faulted_window = 0;
  for (const auto window : both_windows)
    if (window != healthy_windows.front())
      faulted_window = static_cast<::Window>(window);
  if (!expect(faulted_window != 0, "faulted window is distinguishable from healthy")) return;
  XDestroyWindow(display->raw(), faulted_window);
  XSync(display->raw(), False);
  expect(!backend.show("faulted", frame), "retained fixture update fails");
  const std::vector<std::string> retained{"healthy"};
  backend.retain(retained);
  expect(backend.unavailable().empty(), "retaining only healthy clears the removed fault");
  expect(backend.windows().size() == 1, "the healthy retained window remains owned");

  backend.retain({});
  if (!expect(backend.show("", frame), "empty overlay key can be owned")) return;
  const auto empty_key_windows = backend.windows();
  if (!expect(empty_key_windows.size() == 1, "one empty-key fixture exists")) return;
  XDestroyWindow(display->raw(), static_cast<::Window>(empty_key_windows.front()));
  XSync(display->raw(), False);
  expect(!backend.show("", frame), "empty-key fixture update fails");
  expect(!backend.unavailable().empty(), "empty-key failure is visible");
  backend.retain({});
  expect(backend.unavailable().empty(), "empty retention clears even an empty-key fault");
  expect(backend.windows().empty(), "empty retention releases the empty-key fixture");
}

void multiple_update_failures() {
  using layerc::test::expect;
  layerc::test::require_x11();
  std::string error;
  auto display = X11Display::open(error);
  if (!expect(display.has_value(), "open simultaneous-failure display")) return;
  const auto frame = bar_frame(96, 1, {0, 0, 400, 300});
  enum class Resolution { recover, hide, remove };
  for (const bool empty_key : {false, true}) {
    const std::string first_key = empty_key ? "" : "fault-a";
    for (const auto resolution : {Resolution::recover, Resolution::hide,
                                  Resolution::remove}) {
      layerc::daemon::X11OverlayBackend backend(*display);
      if (!expect(backend.show(first_key, frame) && backend.show("fault-b", frame),
                  "fault isolation: create both owned overlay fixtures")) return;
      const auto windows = backend.windows();
      if (!expect(windows.size() == 2, "fault isolation: two owned windows")) return;
      for (const auto window : windows)
        XDestroyWindow(display->raw(), static_cast<::Window>(window));
      XSync(display->raw(), False);
      expect(!backend.show(first_key, frame), "fault isolation: first update fails");
      expect(!backend.show("fault-b", frame), "fault isolation: second update fails");
      expect(backend.unavailable().size() == 2,
             "fault isolation: retain both unresolved backend reasons");
      if (resolution == Resolution::recover) {
        expect(backend.show("fault-b", frame), "fault isolation: recover only second target");
      } else if (resolution == Resolution::hide) {
        backend.hide("fault-b");
      } else {
        const std::vector<std::string> retained{first_key};
        backend.retain(retained);
      }
      expect(backend.unavailable().size() == 1,
             "fault isolation: resolving second target preserves first target fault");
      expect(backend.show(first_key, frame), "fault isolation: recover remaining target");
      expect(backend.unavailable().empty(), "fault isolation: final recovery clears final fault");
      backend.retain({});
      expect(backend.windows().empty() && backend.unavailable().empty(),
             "fault isolation: releasing all targets clears windows and faults");
    }
  }
}

void restack_notifications() {
  using layerc::test::expect;
  layerc::test::require_x11();
  std::string error;
  auto display = X11Display::open(error);
  auto observer = X11Display::open(error);
  if (!expect(display.has_value() && observer.has_value(), "open overlay and observer displays"))
    return;
  auto *raw = reinterpret_cast<Display *>(observer->raw());
  const auto root = static_cast<::Window>(observer->root());
  XSelectInput(raw, root, SubstructureNotifyMask);
  XSync(raw, False);
  layerc::daemon::X11OverlayBackend backend(*display);
  auto frame = bar_frame(96, 1, {0, 0, 400, 300});
  frame.anchor_active = true;
  expect(backend.show("restack", frame), "initial overlay maps raised");
  const auto windows = backend.windows();
  if (!expect(windows.size() == 1, "one overlay exists")) return;
  const auto window = static_cast<::Window>(windows.front());
  const auto configurations = [&] {
    XSync(display->raw(), False);
    XSync(raw, False);
    int count = 0;
    while (XPending(raw) > 0) {
      XEvent event{};
      XNextEvent(raw, &event);
      if (event.type == ConfigureNotify && event.xconfigure.window == window)
        ++count;
    }
    return count;
  };
  (void)configurations();
  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  const auto cover = XCreateWindow(raw, root, 0, 0, 400, 300, 0, CopyFromParent,
                                   InputOutput, CopyFromParent, CWOverrideRedirect,
                                   &attributes);
  XMapRaised(raw, cover);
  XSync(raw, False);
  (void)configurations();
  for (int index = 0; index < 5; ++index)
    expect(backend.show("restack", frame), "repaint succeeds without restacking");
  expect(configurations() == 0, "repeated show does not raise an existing overlay");
  backend.restack();
  expect(backend.show("restack", frame), "show after restack succeeds");
  expect(configurations() == 1, "restack raises exactly once on the next show");
  for (int index = 0; index < 5; ++index)
    expect(backend.show("restack", frame), "repaint after restack succeeds");
  expect(configurations() == 0, "generation is consumed by the first raise");
  XDestroyWindow(raw, cover);
  XSync(raw, False);
}

int main() {
  return layerc::test::run({
      {"scale_rules", [] {
         bool passed = true;
         check_scale_rules(passed);
       }},
      {"geometry_rules", [] {
         bool passed = true;
         check_geometry_rules(passed);
       }},
      {"missing_shape_path", [] {
         bool passed = true;
         check_missing_shape_path(passed);
       }},
      {"restack_notifications", restack_notifications},
      {"update_failure_recovery", update_failure_recovery},
      {"multiple_update_failures", multiple_update_failures},
      {"live_overlay", [] {
         layerc::test::require_x11();
         const int result = run_linux_x11_overlay_tests();
         if (result == skip_code) layerc::test::skip();
         layerc::test::expect(result == 0, "the X11 overlay program should succeed");
       }}
  });
}
