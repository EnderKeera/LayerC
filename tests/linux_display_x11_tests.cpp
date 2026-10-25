#include "display_adapters.hpp"
#include "session_engine.hpp"
#include "x11_provider.hpp"
#include "test_support.hpp"

#include <type_traits>

namespace {
template <typename T>
constexpr bool pinned_provider = !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> && !std::is_move_constructible_v<T> &&
    !std::is_move_assignable_v<T>;
static_assert(pinned_provider<layerc::daemon::X11Provider>);
static_assert(pinned_provider<layerc::daemon::X11OverlayBackend>);
static_assert(pinned_provider<layerc::daemon::X11CaptureFactory>);
using layerc::test::expect;
void unchanged_x11_provider() {
  layerc::test::require_x11();
  const layerc::test::ScopedEnvironment backend("LAYERC_DISPLAY_BACKEND", "x11");
  const layerc::test::ScopedEnvironment no_wl("WAYLAND_DISPLAY", std::nullopt);
  const layerc::test::ScopedEnvironment no_socket("WAYLAND_SOCKET", std::nullopt);
  layerc::daemon::X11Provider provider;
  layerc::daemon::SessionEngine baseline;
  layerc::daemon::DisplayAdapters current;
  layerc::runtime::DaemonKernel old_kernel(baseline.runtime_services());
  layerc::runtime::DaemonKernel kernel(current.runtime_services());
  layerc::platform::EventLoop old_loop, loop;
  layerc::daemon::LinuxStatusSnapshot old_status, status;
  for (std::size_t i = 0; i < layerc::runtime::capability_count; ++i)
    old_status.capabilities.push_back({static_cast<layerc::runtime::Capability>(i),
        layerc::runtime::CapabilityAvailability::unavailable, {}, {}});
  status.capabilities = old_status.capabilities;
  std::vector<std::string> old_diagnostics, diagnostics;
  provider.start(old_loop, old_status, old_diagnostics);
  current.start(loop, status, diagnostics);
  expect(status.x11.connected && status.x11 == old_status.x11, "seam preserves every native X11 probe");
  const layerc::daemon::DisplayRoute route{
      .windows = &provider, .monitors = &provider,
      .overlay = provider.overlay(), .x11_capture = provider.capture(),
      .inputs = {&provider}};
  baseline.refresh({}, true, route, old_kernel, old_status, old_diagnostics);
  current.refresh({}, true, kernel, status, diagnostics);
  expect(status.capabilities == old_status.capabilities && old_diagnostics == diagnostics,
         "X11 availability, demand, diagnostics and session behavior stay unchanged");
  expect(status.display && status.display->session == layerc::runtime::DisplaySession::x11 &&
         status.display->providers.windows.name == "x11_ewmh" &&
         status.display->providers.overlay.name == "x11_shape" &&
         !status.display->wayland.connected,
         "forced X11 exposes the existing provider group and no Wayland connection");
  std::string old_json, new_json;
  const auto old_document = std::get<layerc::core::json::Value>(
      layerc::core::json::parse(layerc::daemon::status_payload_json(old_kernel, old_status)));
  const auto document = std::get<layerc::core::json::Value>(
      layerc::core::json::parse(layerc::daemon::status_payload_json(kernel, status)));
  layerc::core::json::append_value(old_json, *old_document.find("x11"));
  layerc::core::json::append_value(new_json, *document.find("x11"));
  expect(old_json == new_json, "X11 object remains byte-identical through the new seam");
  old_kernel.quiesce();
  kernel.quiesce();
}
} // namespace
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(unchanged_x11_provider))
