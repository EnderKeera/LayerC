// Click-through Shape overlays and monitor scale (V2.6.1). Runs against
// Xvfb: an overlay window is created, its ShapeInput region must stay empty,
// and the painted pixels must match the panel's colors. Returns 77 (ctest
// skip) when no $DISPLAY is reachable.
//
// The geometry and scale rules are pure and are checked before the server is
// touched, so a headless environment still verifies the math.
#include "input_service.hpp"
#include "overlay_service.hpp"
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
  scan.randr = true;
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
}

[[nodiscard]] std::string fixture_profile(std::string_view executable_name) {
  std::string json =
      R"json({"schemaVersion":4,"modules":[],"hidBindings":[],"cooldownGroups":[],"id":"overlay-fixture","name":"Overlay Fixture","game":{"executableName":"NAME"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":60000,"recovery":"single"}],"panels":[{"id":"main","name":"Main","style":"bar","bar":{"width":260,"height":34,"fillColor":"#50bac8","backgroundColor":"#272d35","borderColor":"#50bac8","textColor":"#e4e8ec","showText":true,"hideWhenReady":true},"anchorX":0.5,"anchorY":0.5,"cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]})json";
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
  pressed.source_filter = "keyboard:69";
  pressed.actions = {hide};

  layerc::core::AutomationAction clear;
  clear.kind = AutomationActionKind::clear_overlay_override;
  clear.target = "hide";

  layerc::core::AutomationRule released;
  released.id = "released";
  released.event = AutomationEventKind::input_released;
  released.source_filter = "keyboard:69";
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
    layerc::daemon::X11OverlayService disabled;
    disabled.attach(*display);
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
  passed &= expect(monitors.randr, "the monitor scan used RandR");
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
  layerc::daemon::X11SessionRegistry input_sessions;
  layerc::daemon::X11InputService input(input_sessions);
  layerc::daemon::X11OverlayService overlay;
  input.attach(*display);
  overlay.attach(*display);
  const X11WindowScan scan = layerc::platform::X11WindowTracker(*display).scan();
  layerc::test::TempDirectory temp{"layerc-x11-overlay"};
  const StoredProfile profiles[] = {
      {.path = temp.path() / "profile.json", .profile = *profile}};
  input_sessions.refresh(profiles, scan);
  passed &= expect(input_sessions.session_count() == 1, "the fixture window matches");

  auto background_scan = scan;
  background_scan.active.reset();
  for (auto &snapshot : background_scan.windows) snapshot.active = false;
  input_sessions.refresh(profiles, background_scan);
  passed &= expect(input_sessions.session_count() == 1 &&
                       !input_sessions.sessions().front().active,
                   "a background profile keeps an inactive session");
  overlay.refresh(input_sessions.sessions(), background_scan, monitors);
  passed &= expect(overlay.resources().objects == 0,
                   "an inactive session creates no overlay target");

  input_sessions.refresh(profiles, scan);
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
  passed &= expect(overlay.window_count() == 1, "one panel -> one window");
  passed &= expect(overlay.unavailable().empty(),
                   "the overlay has no unavailability reason");
  const auto windows = overlay.windows();
  passed &= expect(windows.size() == 1, "the overlay publishes its windows");
  if (windows.empty()) return passed ? 0 : 1;
  const auto overlay_window = static_cast<::Window>(windows.front());

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
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : input_sessions.sessions())
    (void)session.cooldowns.trigger("dash", now);
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
  session.automation_runtime = std::make_unique<layerc::core::AutomationRuntime>(
      overlay_automation(session.profile.id));
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
    (void)session.cooldowns.trigger("dash", ready);
  const auto running = ready + std::chrono::seconds{10};
  overlay.render(input_sessions.sessions(), running);
  XGetWindowAttributes(raw, overlay_window, &attributes);
  passed &= expect(attributes.map_state == IsViewable,
                   "a running cooldown shows the panel again");

  // Visibility automation hides the winning panel, then clearing the layer
  // rebuilds the frame from the profile's visible base state.
  const layerc::core::InputEvent press_e{
      {layerc::core::InputDevice::keyboard, 69, 0}, true};
  const layerc::core::InputEvent release_e{
      {layerc::core::InputDevice::keyboard, 69, 0}, false};
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
      {"live_overlay", [] {
         layerc::test::require_x11();
         const int result = run_linux_x11_overlay_tests();
         if (result == skip_code) layerc::test::skip();
         layerc::test::expect(result == 0, "the X11 overlay program should succeed");
       }}
  });
}
