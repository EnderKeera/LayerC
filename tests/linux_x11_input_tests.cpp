// XI2 passive input, XKB normalization and profile sessions (V2.6.1). Runs
// against Xvfb: XTest injects real events that the XI2 path must normalize.
// Returns 77 (ctest skip) when no $DISPLAY is reachable.
//
// XTest is linked here and only here — layerd reads input, never synthesizes
// it, and does not link the extension at all.
#include "input_service.hpp"
#include "overlay_service.hpp"
#include "x11_display.hpp"
#include "x11_input.hpp"
#include "x11_monitor.hpp"
#include "x11_window.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using layerc::core::GameProfile;
using layerc::platform::StoredProfile;
using layerc::platform::X11Display;
using layerc::platform::X11InputSource;
using layerc::platform::X11WindowScan;
using layerc::platform::X11WindowSnapshot;

using layerc::test::skip_code;

[[nodiscard]] layerc::core::AutomationExpression
literal(layerc::core::AutomationValue value) {
  layerc::core::AutomationExpression expression;
  expression.kind = layerc::core::ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

[[nodiscard]] layerc::core::AutomationProfile
automation_fixture(std::string profile_id) {
  using layerc::core::AutomationActionKind;
  using layerc::core::AutomationEventKind;
  using layerc::core::AutomationValue;
  using layerc::core::AutomationValueType;
  using layerc::core::DurationValue;

  layerc::core::AutomationAction increment;
  increment.kind = AutomationActionKind::increment_variable;
  increment.target = "count";
  increment.value = literal(AutomationValue(std::int64_t{1}));

  layerc::core::AutomationAction create_timer;
  create_timer.kind = AutomationActionKind::create_timer;
  create_timer.target = "pulse";
  create_timer.value = literal(
      AutomationValue(DurationValue{std::chrono::milliseconds{50}}));

  layerc::core::AutomationAction hide_overlay;
  hide_overlay.kind = AutomationActionKind::set_overlay_override;
  hide_overlay.target = "main";
  hide_overlay.name = "hide";
  hide_overlay.property = "visible";
  hide_overlay.value = literal(AutomationValue(false));

  layerc::core::AutomationRule input;
  input.id = "input";
  input.event = AutomationEventKind::input_pressed;
  input.source_filter = "keyboard:69";
  input.actions = {increment, create_timer, hide_overlay};

  layerc::core::AutomationAction trigger_cooldown;
  trigger_cooldown.kind = AutomationActionKind::trigger_cooldown;
  trigger_cooldown.target = "potion";

  layerc::core::AutomationAction clear_overlay;
  clear_overlay.kind = AutomationActionKind::clear_overlay_override;
  clear_overlay.target = "hide";

  layerc::core::AutomationRule timer;
  timer.id = "timer";
  timer.event = AutomationEventKind::timer_fired;
  timer.source_filter = "pulse";
  timer.actions = {increment, trigger_cooldown, clear_overlay};

  auto stopping = input;
  stopping.id = "stopping";
  stopping.event = AutomationEventKind::session_stopping;
  stopping.source_filter.clear();
  auto mark_stopped = increment;
  mark_stopped.target = "stopped";
  stopping.actions = {mark_stopped};

  layerc::core::AutomationProfile profile;
  profile.profile_id = std::move(profile_id);
  profile.variables = {{.id = "count",
                        .type = AutomationValueType::integer,
                        .default_value = AutomationValue(std::int64_t{0}),
                        .persistent = true}};
  profile.variables.push_back({.id = "stopped",
                               .type = AutomationValueType::integer,
                               .default_value = AutomationValue(std::int64_t{0}),
                               .persistent = true});
  profile.automations = {
      {.id = "fixture", .name = "Fixture", .rules = {input, timer, stopping}}};
  return profile;
}

[[nodiscard]] std::string self_executable() {
  return layerc::platform::process_executable_path(::getpid());
}

[[nodiscard]] std::string basename_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] bool raw_input_selected(Display *display, ::Window root) {
  int count{};
  XIEventMask *masks = XIGetSelectedEvents(display, root, &count);
  bool selected = false;
  for (int index = 0; index < count; ++index) {
    if (masks[index].deviceid != XIAllDevices) continue;
    selected = XIMaskIsSet(masks[index].mask, XI_RawKeyPress) ||
               XIMaskIsSet(masks[index].mask, XI_RawButtonPress) ||
               XIMaskIsSet(masks[index].mask, XI_HierarchyChanged);
  }
  if (masks != nullptr) XFree(masks);
  return selected;
}

// One profile: keyboard 69 (VK_E) -> dash, mouse left -> potion, mouse wheel
// up (normalized code 6) -> zoom, controller button 1 -> ultimate. The
// controller binding is deliberately unsupported on an XI2 host without a
// gamepad and must survive untouched.
void replace_all(std::string &text, std::string_view placeholder,
                 std::string_view value) {
  std::size_t at = 0;
  while ((at = text.find(placeholder, at)) != std::string::npos) {
    text.replace(at, placeholder.size(), value);
    at += value.size();
  }
}

[[nodiscard]] std::string fixture_profile(std::string_view id,
                                          std::string_view match_json,
                                          std::string_view dash_id) {
  std::string json = R"json({"schemaVersion":1,"id":"PROFILE_ID","name":"Fixture","game":PROFILE_MATCH,"cooldowns":[{"id":"DASH_ID","name":"Dash","durationMs":60000},{"id":"potion","name":"Potion","durationMs":60000},{"id":"zoom","name":"Zoom","durationMs":60000},{"id":"ultimate","name":"Ultimate","durationMs":60000}],"panels":[{"id":"main","name":"Main","cooldowns":["DASH_ID"]}],"bindings":[{"cooldownId":"DASH_ID","chord":[{"device":"keyboard","code":69}]},{"cooldownId":"potion","chord":[{"device":"mouse","code":1}]},{"cooldownId":"zoom","chord":[{"device":"mouse","code":6}]},{"cooldownId":"ultimate","chord":[{"device":"controller","code":1,"controllerIndex":0}]}]})json";
  replace_all(json, "PROFILE_ID", id);
  replace_all(json, "PROFILE_MATCH", match_json);
  replace_all(json, "DASH_ID", dash_id);
  return json;
}

[[nodiscard]] std::optional<GameProfile> parse(const std::string &json) {
  auto parsed = layerc::core::parse_profile_json(json);
  if (auto *profile = std::get_if<GameProfile>(&parsed))
    return std::move(*profile);
  std::fprintf(stderr, "profile parse failed: %s\njson: %s\n",
               std::get<layerc::core::ProfileError>(parsed).message.c_str(),
               json.c_str());
  return std::nullopt;
}

void check_normalization(bool &passed) {
  using layerc::test::expect;

  // --- normalization (pure) ------------------------------------------------
  using layerc::platform::keysym_to_vk;
  passed &= expect(keysym_to_vk(XK_e) == 0x45, "lowercase e maps to VK_E (69)");
  passed &= expect(keysym_to_vk(XK_A) == 0x41, "uppercase A maps to VK_A");
  passed &= expect(keysym_to_vk(XK_7) == 0x37, "digit 7 maps to VK_7");
  passed &= expect(keysym_to_vk(XK_F5) == 0x74, "F5 maps to VK_F5");
  passed &= expect(keysym_to_vk(XK_Delete) == 0x2E, "Delete maps to VK_DELETE");
  passed &= expect(keysym_to_vk(XK_Left) == 0x25, "Left maps to VK_LEFT");
  passed &= expect(keysym_to_vk(XK_Shift_L) == 0x10 &&
                       keysym_to_vk(XK_Shift_R) == 0x10,
                   "both shifts map to VK_SHIFT");
  passed &= expect(keysym_to_vk(XK_Control_R) == 0x11 &&
                       keysym_to_vk(XK_Alt_L) == 0x12,
                   "control and alt map to VK_CONTROL / VK_MENU");
  passed &= expect(keysym_to_vk(XK_Escape) == 0x1B &&
                       keysym_to_vk(XK_Return) == 0x0D &&
                       keysym_to_vk(XK_space) == 0x20,
                   "escape, return and space map to their VKs");
  passed &= expect(keysym_to_vk(XK_KP_8) == 0x68 && keysym_to_vk(XK_KP_Up) == 0x26,
                   "numpad keys map with and without NumLock");
  passed &= expect(keysym_to_vk(XK_semicolon) == 0xBA,
                   "semicolon maps to VK_OEM_1");
  passed &= expect(!keysym_to_vk(0x0EFF).has_value(),
                   "keysyms without a VK are dropped, not guessed");

  using layerc::platform::button_to_mouse_code;
  passed &= expect(button_to_mouse_code(1) == 1 && button_to_mouse_code(3) == 3,
                   "left and middle buttons keep their code");
  passed &= expect(button_to_mouse_code(4) == 6 && button_to_mouse_code(5) == 7,
                   "X wheel buttons 4/5 map to the profile wheel codes 6/7");
  passed &= expect(button_to_mouse_code(6) == 4 && button_to_mouse_code(7) == 5,
                   "X side buttons 6/7 map to the profile X1/X2 codes");

  using layerc::platform::classify_device;
  using layerc::platform::X11DeviceKind;
  passed &= expect(classify_device("AT Keyboard", true, false, 0, false) ==
                       X11DeviceKind::keyboard,
                   "a device with keys is a keyboard");
  passed &= expect(classify_device("Virtual core XTEST pointer", false, true, 2,
                                   false) == X11DeviceKind::mouse,
                   "a pointer-named button device is a mouse");
  passed &= expect(classify_device("SynPS/2 Synaptics TouchPad", false, true, 2,
                                   false) == X11DeviceKind::mouse,
                   "touchpads are mice");
  passed &= expect(classify_device("Logitech USB Wheel", false, true, 3, true) ==
                       X11DeviceKind::mouse,
                   "a scroll axis means a pointing device");
  passed &= expect(classify_device("Microsoft X-Box 360 pad", false, true, 6,
                                   false) == X11DeviceKind::gamepad,
                   "a button device with axes and no scroll class is a gamepad");
  passed &= expect(classify_device("Pedal", false, true, 0, false) ==
                       X11DeviceKind::other,
                   "buttons without axes are not claimed");
}

} // namespace

int run_linux_x11_input_tests() {
  using layerc::test::expect;
  bool passed = true;

  // --- sessions (pure) -----------------------------------------------------
  const std::string self_name = basename_of(self_executable());
  auto by_name = parse(fixture_profile("by-name",
                                       R"json({"executableName":")json" +
                                           self_name + R"json("})json",
                                       "dash"));
  auto global = parse(fixture_profile("global", R"json({"global":true})json",
                                      "global-dash"));
  passed &= expect(by_name.has_value() && global.has_value(),
                   "fixture profiles parse");
  if (!by_name.has_value() || !global.has_value()) return 1;

  layerc::test::TempDirectory temp{"layerc-x11-input"};
  layerc::test::ScopedEnvironment config_home{"XDG_CONFIG_HOME",
                                               temp.path() / "config"};
  layerc::test::ScopedEnvironment data_home{"XDG_DATA_HOME",
                                             temp.path() / "data"};
  if (!expect(config_home.applied() && data_home.applied(),
              "the test owns isolated XDG roots"))
    return 1;
  const auto roots = layerc::platform::profile_store_roots();
  passed &= expect(layerc::platform::ensure_profile_store_roots(roots),
                   "the isolated profile store is available");

  X11WindowScan scan;
  X11WindowSnapshot window;
  window.window = 0x1234;
  window.executable_name = self_name;
  window.title = "Fixture";
  scan.windows.push_back(window);
  auto foreground = window;
  foreground.window = 0x5678;
  foreground.active = true;
  scan.windows.push_back(foreground);
  scan.active = foreground.window;

  layerc::daemon::X11SessionRegistry service_sessions;

  layerc::daemon::X11InputService service(service_sessions);
  const StoredProfile profiles[] = {
      {.path = roots.config_profiles / "by-name.json", .profile = *by_name},
      {.path = roots.config_profiles / "global.json", .profile = *global}};
  service_sessions.configure(profiles);
  service_sessions.refresh(scan);
  passed &= expect(service_sessions.session_count() == 2,
                   "both profiles resolve against the fixture window");
  passed &= expect(service_sessions.session_requests().size() == 2,
                   "each session publishes a capability request");
  passed &= expect(service_sessions.session_requests().front().global == false &&
                       service_sessions.session_requests().back().global == true,
                   "the global profile reports itself as global");
  passed &= expect(service_sessions.sessions().front().window == foreground.window &&
                       service_sessions.session_requests().front().active,
                   "a non-global profile prefers and reports the active match");
  const auto state_now = layerc::core::MonotonicClock::now();
  passed &= expect(service_sessions.sessions().front().cooldowns.trigger("dash", state_now),
                   "fixture cooldown starts");
  service_sessions.refresh(scan);
  passed &= expect(!service_sessions.sessions().front().cooldowns.trigger("dash", state_now),
                   "an unchanged refresh preserves cooldown state");
  auto changed = profiles[0];
  changed.profile.name = "Changed definition";
  const StoredProfile changed_profiles[] = {changed, profiles[1]};
  service_sessions.configure(changed_profiles);
  service_sessions.refresh(scan);
  passed &= expect(service_sessions.sessions().front().cooldowns.trigger("dash", state_now),
                   "a changed profile definition starts a fresh session");

  X11WindowScan empty_scan;
  service_sessions.configure(profiles);
  service_sessions.refresh(empty_scan);
  passed &= expect(service_sessions.session_count() == 0,
                   "no windows means no sessions");
  service_sessions.refresh(scan);

  X11WindowScan background_scan = scan;
  background_scan.active = 0x9999;
  for (auto &snapshot : background_scan.windows) snapshot.active = false;
  service_sessions.refresh(background_scan);
  passed &= expect(!service_sessions.session_requests().front().active &&
                       service_sessions.session_requests().back().active,
                   "only global profiles stay active in the background");
  passed &= expect(service_sessions.sessions().front().window == foreground.window,
                   "focus loss retains the previous matching window");
  passed &= expect(layerc::platform::match_window(
                       scan, profiles[0].profile.game, window.window)->window ==
                       foreground.window,
                   "an active match wins over a previous background target");
  auto fallback_scan = background_scan;
  auto unmatched = window;
  unmatched.window = 0x9999;
  unmatched.executable_name = "different-program";
  fallback_scan.windows.push_back(unmatched);
  passed &= expect(layerc::platform::match_window(
                       fallback_scan, profiles[0].profile.game,
                       unmatched.window)->window == window.window,
                   "an unmatched previous target preserves normal fallback order");
  // F3 regression: focus-away/back with automation enabled must not
  // reconstruct the session (which would reset the cooldown engine).
  {
    const auto focus_now = layerc::core::MonotonicClock::now();
    passed &= expect(service_sessions.sessions().front().cooldowns.trigger("dash", focus_now),
                     "focus regression starts a cooldown");
    service_sessions.refresh(background_scan);
    passed &= expect(!service_sessions.sessions().front().cooldowns.trigger("dash", focus_now),
                     "a background refresh preserves cooldown state");
    service_sessions.refresh(scan);
    passed &= expect(!service_sessions.sessions().front().cooldowns.trigger("dash", focus_now),
                     "focus gain preserves cooldown state");
  }
  service_sessions.refresh(scan);

  layerc::daemon::InputSession session(profiles[0], window.window, false);
  const layerc::core::InputEvent press_e{
      {layerc::core::InputDevice::keyboard, 69, 0}, true};
  const layerc::core::InputEvent release_e{
      {layerc::core::InputDevice::keyboard, 69, 0}, false};
  const auto now = layerc::core::MonotonicClock::now();
  auto triggered = session.feed(press_e, now);
  passed &= expect(triggered.size() == 1 && triggered.front() == "dash",
                   "the keyboard binding triggers its cooldown");
  passed &= expect(session.feed(press_e, now).empty(),
                   "a running cooldown does not retrigger");
  passed &= expect(session.feed(release_e, now).empty(),
                   "the release of a single-key chord triggers nothing");
  triggered = session.feed({{layerc::core::InputDevice::mouse, 1, 0}, true}, now);
  passed &= expect(triggered.size() == 1 && triggered.front() == "potion",
                   "the mouse binding triggers its cooldown");
  triggered = session.feed(
      {{layerc::core::InputDevice::controller, 1, 0}, true}, now);
  passed &= expect(triggered.size() == 1 && triggered.front() == "ultimate",
                   "controller bindings survive the round trip");
  passed &= expect(session.profile.bindings.size() == 4,
                   "every binding stays in the session");

  // Retain one window across focus changes, including when another global
  // consumer keeps XI2 attached and background releases are not dispatched.
  {
    auto focus_scan = scan;
    focus_scan.windows.erase(focus_scan.windows.begin());
    auto inactive_scan = focus_scan;
    inactive_scan.active = 0;
    inactive_scan.windows.front().active = false;
    auto chord_profile = profiles[0];
    chord_profile.profile.bindings.front().chord.tokens.push_back(
        {layerc::core::InputDevice::keyboard, 17, 0});
    layerc::daemon::X11SessionRegistry focus_service_sessions;
    layerc::daemon::X11InputService focus_service(focus_service_sessions);
    focus_service_sessions.configure(std::span{&chord_profile, 1});
    focus_service_sessions.refresh(focus_scan);
    const layerc::core::InputEvent ctrl{
        {layerc::core::InputDevice::keyboard, 17, 0}, true};
    (void)focus_service_sessions.sessions().front().matcher.process(ctrl);
    focus_service_sessions.refresh(inactive_scan);
    focus_service_sessions.refresh(focus_scan);
    passed &= expect(
        focus_service_sessions.sessions().front().matcher.process(press_e).empty(),
        "focus loss clears a modifier whose background release was skipped");

    focus_service_sessions.configure(std::span{profiles}.first(1));
    focus_service_sessions.refresh(focus_scan);
    passed &= expect(
        focus_service_sessions.sessions().front().matcher.process(press_e).size() == 1,
        "single-key fixture latches a press");
    focus_service_sessions.refresh(focus_scan);
    passed &= expect(
        focus_service_sessions.sessions().front().matcher.process(press_e).empty(),
        "an unchanged active refresh keeps the held key latched");
    focus_service_sessions.refresh(inactive_scan);
    focus_service_sessions.refresh(focus_scan);
    passed &= expect(
        focus_service_sessions.sessions().front().matcher.process(press_e).size() == 1,
        "focus loss clears a single key whose release was skipped");
  }

  // --- automation sidecar/runtime -----------------------------------------
  const auto automation_profile = automation_fixture(profiles[0].profile.id);
  passed &= expect(layerc::platform::write_profile_bundle(
                       profiles[0].path, profiles[0].profile, std::nullopt,
                       automation_profile),
                   "the automation sidecar is committed beside its profile");
  service_sessions.configure(profiles);
  service_sessions.refresh(scan);
  passed &= expect(service_sessions.automation_count() == 1 &&
                       service_sessions.automation_diagnostics().empty(),
                   "an active matched profile owns one sidecar runtime");
  auto &automated = service_sessions.sessions().front();
  const auto automation_now = layerc::core::MonotonicClock::now();
  const auto automation_triggered = automated.feed(press_e, automation_now);
  passed &= expect(automation_triggered.size() == 1 &&
                       automation_triggered.front() == "dash",
                   "automation observes input before the binding cooldown");
  passed &= expect(automated.automation_runtime != nullptr &&
                       automated.automation_runtime->snapshot().timers.size() == 1 &&
                       automated.automation_runtime->snapshot().overlay_overrides.size() ==
                           1,
                   "input commits timer and overlay effects");
  const auto state_due = service_sessions.next_automation_deadline(automation_now);
  passed &= expect(state_due.has_value(),
                   "runtime effects publish a monotonic service deadline");

  (void)service_sessions.poll_automation(
      automation_now + std::chrono::milliseconds{55});
  const auto &after_timer = automated.automation_runtime->snapshot();
  const auto count = after_timer.persistent_values.find("count");
  passed &= expect(count != after_timer.persistent_values.end() &&
                       count->second == layerc::core::AutomationValue(
                                            std::int64_t{2}) &&
                       after_timer.timers.empty() &&
                       after_timer.overlay_overrides.empty() &&
                       !automated.cooldowns.trigger(
                           "potion", automation_now +
                                         std::chrono::milliseconds{55}),
                   "the timer commits persistent, cooldown, and override effects");

  (void)service_sessions.poll_automation(
      automation_now + std::chrono::milliseconds{1'100});
  const auto persisted = layerc::platform::load_automation_state(
      profiles[0].path, automation_profile);
  const auto persisted_count = persisted.values.find("count");
  passed &= expect(!persisted.error.has_value() &&
                       persisted_count != persisted.values.end() &&
                       persisted_count->second == layerc::core::AutomationValue(
                                                      std::int64_t{2}),
                   "dirty persistent state checkpoints transactionally");

  layerc::daemon::X11SessionRegistry reloaded_sessions;

  layerc::daemon::X11InputService reloaded(reloaded_sessions);
  reloaded_sessions.configure(std::span{profiles}.first(1));
  reloaded_sessions.refresh(scan);
  const auto &reloaded_values =
      reloaded_sessions.sessions().front().automation_runtime->snapshot().persistent_values;
  passed &= expect(reloaded_sessions.automation_count() == 1 &&
                       reloaded_values.at("count") ==
                           layerc::core::AutomationValue(std::int64_t{2}),
                   "a fresh runtime reloads its persistent values");

  // --- live XI2 path -------------------------------------------------------
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
  auto source = X11InputSource::create(*display, error);
  if (!source.has_value()) {
    std::fprintf(stderr, "xi2 source: %s\n", error.c_str());
    return 1;
  }
  passed &= expect(source->valid(), "XI2 raw events are selected");
  passed &= expect(!source->devices().empty(), "the device roster is populated");
  const auto caps = source->capabilities();
  passed &= expect(caps.keyboard && caps.mouse,
                   "Xvfb exposes a keyboard and a pointer");
  passed &= expect(!caps.gamepad, "Xvfb exposes no gamepad");
  bool gamepad_reason = false;
  for (const auto &reason : source->unavailable())
    if (reason.find("gamepad") != std::string::npos) gamepad_reason = true;
  passed &= expect(gamepad_reason,
                   "the missing gamepad is reported as a capability reason");
  passed &= expect(source->unavailable({.keyboard = true, .mouse = true,
                                        .gamepad = false})
                       .empty(),
                   "an unneeded gamepad class is not a capability reason");
  passed &= expect(!source->refresh_devices() && !source->refresh_devices(),
                   "rescanning a quiet roster reports no change");

  auto *raw = reinterpret_cast<Display *>(display->raw());
  const ::Window root = DefaultRootWindow(raw);
  const ::Window fixture = XCreateSimpleWindow(raw, root, 5, 5, 120, 80, 0, 0, 0);
  XMapWindow(raw, fixture);
  const unsigned long pid = static_cast<unsigned long>(::getpid());
  XChangeProperty(raw, fixture, XInternAtom(raw, "_NET_WM_PID", False),
                  XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&pid), 1);
  XChangeProperty(raw, root, XInternAtom(raw, "_NET_ACTIVE_WINDOW", False),
                  XA_WINDOW, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&fixture), 1);
  XSync(raw, False);

  passed &= expect(raw_input_selected(raw, root),
                   "the XI2 source owns its raw-event selection");
  source.reset();
  passed &= expect(!raw_input_selected(raw, root),
                   "destroying the XI2 source clears raw-event selection");

  layerc::platform::X11WindowTracker tracker(*display);
  layerc::daemon::X11SessionRegistry live_registry;
  layerc::daemon::X11InputService live(live_registry);
  live.attach(*display);
  live_registry.configure(profiles);
  live_registry.refresh(tracker.scan());
  const auto live_sessions = live_registry.session_count();
  passed &= expect(live_sessions == 2,
                   "the fixture window resolves both live sessions");
  if (live_sessions != 2) std::fprintf(stderr, "live sessions: %zu\n", live_sessions);

  // The overlay service is registered alongside input: an active session
  // demands both, and this regression must prove input survives that activation.
  layerc::daemon::X11OverlayService overlay;
  overlay.attach(*display);
  overlay.refresh(live_registry.sessions(), tracker.scan(),
                  layerc::platform::scan_monitors(*display));
  const auto requests = live_registry.session_requests();
  const auto demand =
      layerc::runtime::calculate_capability_demand(requests, false);
  layerc::daemon::X11AutomationService automation(live_registry);
  layerc::runtime::RuntimeServiceRegistry services{};
  services[static_cast<std::size_t>(layerc::runtime::Capability::input)] = &live;
  services[static_cast<std::size_t>(layerc::runtime::Capability::automation)] =
      &automation;
  services[static_cast<std::size_t>(layerc::runtime::Capability::overlay)] =
      &overlay;
  layerc::runtime::DaemonKernel kernel(services);
  (void)kernel.reconcile(demand);
  passed &= expect(live.attached(), "demand activates the XI2 input service");
  passed &= expect(kernel.services().find(layerc::runtime::Capability::input) !=
                       nullptr,
                   "the kernel hosts the input service");
  passed &= expect(kernel.services().find(
                       layerc::runtime::Capability::overlay) != nullptr,
                   "the overlay service activates without unwinding input");
  passed &= expect(
      kernel.demand().consumers(layerc::runtime::Capability::automation) == 1 &&
          kernel.services().find(layerc::runtime::Capability::automation) !=
              nullptr &&
          kernel.services()
                  .stats(layerc::runtime::Capability::automation)
                  .resources.objects == 1,
      "one active runtime owns one demanded automation resource");
  // Card panels are visible while ready, so the overlay paints without
  // consuming the cooldowns the injected keypress below must trigger.
  overlay.render(live_registry.sessions(), layerc::core::MonotonicClock::now());
  passed &= expect(overlay.window_count() >= 1, "the overlay shows a panel");

  int xtest_event{};
  int xtest_error{};
  int xtest_major{};
  int xtest_minor{};
  const bool xtest = XTestQueryExtension(raw, &xtest_event, &xtest_error,
                                         &xtest_major, &xtest_minor) == True;
  passed &= expect(xtest, "XTest is available for event injection");
  if (xtest) {
    const KeyCode key_e = XKeysymToKeycode(raw, XK_e);
    XTestFakeKeyEvent(raw, key_e, True, CurrentTime);
    XSync(raw, False);
    auto dispatch = live.dispatch();
    passed &= expect(dispatch.events >= 1, "the injected keypress is received");
    bool got_dash = false;
    bool got_global = false;
    for (const auto &cooldown : dispatch.triggered) {
      if (cooldown == "dash") got_dash = true;
      if (cooldown == "global-dash") got_global = true;
    }
    passed &= expect(got_dash && got_global,
                     "the keypress drives both profile cooldowns");

    XTestFakeKeyEvent(raw, key_e, False, CurrentTime);
    XSync(raw, False);
    dispatch = live.dispatch();
    passed &= expect(dispatch.events >= 1 && dispatch.triggered.empty(),
                     "the key release triggers nothing");

    XTestFakeButtonEvent(raw, 1, True, CurrentTime);
    XSync(raw, False);
    dispatch = live.dispatch();
    bool got_potion = false;
    for (const auto &cooldown : dispatch.triggered)
      if (cooldown == "potion") got_potion = true;
    passed &= expect(got_potion, "the injected mouse button drives a cooldown");
    XTestFakeButtonEvent(raw, 1, False, CurrentTime);
    XSync(raw, False);
    (void)live.dispatch();

    XTestFakeButtonEvent(raw, 4, True, CurrentTime);
    XSync(raw, False);
    dispatch = live.dispatch();
    bool got_zoom = false;
    for (const auto &cooldown : dispatch.triggered)
      if (cooldown == "zoom") got_zoom = true;
    passed &= expect(got_zoom,
                     "X wheel button 4 normalizes to the profile wheel code");
    XTestFakeButtonEvent(raw, 4, False, CurrentTime);
    XSync(raw, False);
    (void)live.dispatch();
  }

  (void)live_registry.sessions().front().matcher.process(press_e);
  kernel.quiesce();
  passed &= expect(!live.attached(), "quiescing releases the XI2 source");
  passed &= expect(!raw_input_selected(raw, root),
                   "quiescing deselects XI2 raw events");
  passed &= expect(live_registry.automation_count() == 1 && kernel.demand().empty() &&
                       kernel.services().aggregate_resources() ==
                           layerc::runtime::RuntimeResourceStats{},
                   "temporary quiescence retains sessions but releases demand");
  passed &= expect(
      live_registry.sessions().front().matcher.process(press_e).size() == 1,
      "detaching XI2 clears transient key state");
  passed &= expect(
      live_registry.sessions().front().automation_runtime->snapshot().persistent_values.at(
          "stopped") == layerc::core::AutomationValue(std::int64_t{0}),
      "temporary quiescence does not run session-stopping rules");
  live_registry.stop_sessions(layerc::core::MonotonicClock::now());
  live_registry.stop_sessions(layerc::core::MonotonicClock::now());
  const auto stopped_state = layerc::platform::load_automation_state(
      profiles[0].path, automation_profile);
  passed &= expect(
      live_registry.automation_count() == 0 && live_registry.session_count() == 0 &&
          !stopped_state.error.has_value() &&
          stopped_state.values.at("stopped") ==
              layerc::core::AutomationValue(std::int64_t{1}),
      "final shutdown runs and persists session-stopping effects exactly once");

  // Discovery remains selected without input demand and turns a property
  // event into a rescan hint instead of discarding it.
  XChangeProperty(raw, root, XInternAtom(raw, "LAYERC_TEST_PROBE", False),
                  XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(&pid), 1);
  XSync(raw, False);
  passed &= expect(tracker.poll_changes(),
                   "window discovery remains active without XI2 demand");

  XDestroyWindow(raw, fixture);
  XDeleteProperty(raw, root, XInternAtom(raw, "LAYERC_TEST_PROBE", False));
  XDeleteProperty(raw, root, XInternAtom(raw, "_NET_ACTIVE_WINDOW", False));
  XSync(raw, False);

  return passed ? 0 : 1;
}

int main() {
  return layerc::test::run({
      {"normalization", [] {
         bool passed = true;
         check_normalization(passed);
       }},
      {"sessions_and_live_input", [] {
         const int result = run_linux_x11_input_tests();
         if (result == skip_code) layerc::test::skip();
         layerc::test::expect(result == 0, "the X11 input program should succeed");
       }}
  });
}
