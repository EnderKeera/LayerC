#include "display_adapters.hpp"
#include "display_selection.hpp"
#include "layerc/runtime/diagnostics.hpp"
#include "test_support.hpp"

#include <algorithm>

namespace {
using namespace layerc::runtime;
using layerc::test::expect;

void selection_policy() {
  using enum DisplayBackend;
  expect(layerc::daemon::display_backend("") == auto_detect &&
         layerc::daemon::display_backend("auto") == auto_detect &&
         layerc::daemon::display_backend("x11") == x11 &&
         layerc::daemon::display_backend("wayland") == wayland &&
         layerc::daemon::display_backend("X11") == invalid,
         "only supported override values select a backend");
  for (const bool wl : {false, true}) for (const bool x : {false, true}) {
    expect(layerc::daemon::select_display_session(auto_detect, wl, x) ==
        (wl ? DisplaySession::wayland : x ? DisplaySession::x11 : DisplaySession::none),
        "auto selects successful Wayland, then X11, then headless");
    expect(layerc::daemon::select_display_session(wayland, wl, x) ==
        (wl ? DisplaySession::wayland : DisplaySession::none), "forced Wayland never selects X11");
    expect(layerc::daemon::select_display_session(x11, wl, x) ==
        (x ? DisplaySession::x11 : DisplaySession::none), "forced X11 never selects Wayland");
    expect(layerc::daemon::select_display_session(invalid, wl, x) == DisplaySession::none,
        "invalid override cannot silently grant a display backend");
  }
}
void compositor_policy() {
  using layerc::daemon::detect_compositor;
  expect(detect_compositor("Plasma:kde", false, false, false, false) == DisplayCompositor::kwin &&
         detect_compositor("ubuntu:GNOME", false, false, false, false) == DisplayCompositor::gnome &&
         detect_compositor("", true, false, false, false) == DisplayCompositor::kwin &&
         detect_compositor("", false, true, false, false) == DisplayCompositor::gnome &&
         detect_compositor("", false, false, true, true) == DisplayCompositor::wlroots &&
         detect_compositor("", false, false, true, false) == DisplayCompositor::unknown,
         "owners, desktop hints and paired wlroots globals select the compositor");
}
void headless_and_invalid_overrides() {
  const layerc::test::ScopedEnvironment no_x("DISPLAY", std::nullopt);
  const layerc::test::ScopedEnvironment no_wl("WAYLAND_DISPLAY", std::nullopt);
  const layerc::test::ScopedEnvironment no_socket("WAYLAND_SOCKET", std::nullopt);
  for (const auto backend : {"auto", "x11", "wayland", "invalid"}) {
    const layerc::test::ScopedEnvironment selection("LAYERC_DISPLAY_BACKEND", backend);
    layerc::daemon::DisplayAdapters adapters;
    layerc::runtime::DaemonKernel kernel(adapters.runtime_services());
    layerc::platform::EventLoop loop;
    LinuxStatusSnapshot status;
    status.x11.reason = "platform_unsupported";
    for (std::size_t i = 0; i < capability_count; ++i)
      status.capabilities.push_back({static_cast<Capability>(i), CapabilityAvailability::unavailable, {}, {}});
    std::vector<std::string> diagnostics;
    adapters.start(loop, status, diagnostics);
    adapters.refresh({}, true, kernel, status, diagnostics);
    expect(status.display && status.display->session == DisplaySession::none && !status.x11.connected,
           "missing display connections retain headless store/control operation");
    expect(adapters.session_count() == 0 && !adapters.has_pending_events() &&
           !adapters.next_deadline(layerc::core::MonotonicClock::now()),
           "headless display seam stays idle without a retry timer");
    adapters.refresh_status(kernel, status);
    const auto parsed = parse_runtime_diagnostics_json(layerc::daemon::status_payload_json(kernel, status));
    const auto *value = std::get_if<RuntimeDiagnostics>(&parsed);
    expect(value && value->linux_extension->display == status.display, "native seam emits typed display status");
    if (std::string_view(backend) == "invalid")
      expect(std::ranges::find(status.display->reasons, "invalid_display_backend") != status.display->reasons.end(),
             "invalid override retains a stable visible reason");
    const auto a = adapters.dispatch({});
    const auto b = adapters.dispatch_pending();
    expect(a.events == 0 && b.events == 0 && !adapters.render(layerc::core::MonotonicClock::now()),
           "headless dispatch and rendering produce no external effects");
    kernel.quiesce();
    adapters.stop_sessions(layerc::core::MonotonicClock::now());
  }
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(selection_policy),
                 LAYERC_TEST_CASE(compositor_policy),
                 LAYERC_TEST_CASE(headless_and_invalid_overrides))
