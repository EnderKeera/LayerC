#include "display_adapters.hpp"
#include "wayland_connection.hpp"
#include "test_support.hpp"
#include "layerc/runtime/diagnostics.hpp"

#ifdef LAYERC_TEST_XWAYLAND_LOSS
#include "x11_display.hpp"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <sys/wait.h>

#include <array>
#include <cstring>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <utility>

namespace {
using layerc::test::expect;
using layerc::test::ScopedEnvironment;

// This is a disposable, in-process protocol server, not a real desktop.
// Threads belong only to the test fixture; product transports remain on the
// daemon's single EventLoop.
class Server {
public:
  enum class InvalidGlobal { none, empty, utf8, zero };
  static int compositor_version() noexcept { return std::min(6, wl_compositor_interface.version); }
  static int output_version() noexcept { return std::min(4, wl_output_interface.version); }
  using Announcements = std::vector<std::pair<std::string, int>>;
  explicit Server(const InvalidGlobal invalid = InvalidGlobal::none, const Announcements &extra = {}) {
    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return;
    display_ = wl_display_create();
    if (!display_) { ::close(sockets[0]); ::close(sockets[1]); return; }
    if (!wl_client_create(display_, sockets[0])) {
      ::close(sockets[0]); ::close(sockets[1]); return;
    }
    client_ = sockets[1];
    const auto bind = [](wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
      (void)wl_resource_create(client, &wl_compositor_interface, static_cast<int>(version), id);
    };
    auto *compositor = wl_global_create(display_, &wl_compositor_interface, compositor_version(), nullptr, bind);
    output_ = wl_global_create(display_, &wl_output_interface, output_version(), nullptr, bind);
    static const wl_interface layer_shell{"zwlr_layer_shell_v1", 4, 0, nullptr, 0, nullptr};
    static const wl_interface foreign_toplevel{"zwlr_foreign_toplevel_manager_v1", 3, 0, nullptr, 0, nullptr};
    auto *layer = wl_global_create(display_, &layer_shell, 4, nullptr, bind);
    auto *toplevel = wl_global_create(display_, &foreign_toplevel, 3, nullptr, bind);
    if (!expect(compositor && output_ && layer && toplevel, "create supported-version fixture globals")) return;
    static const wl_interface empty{"", 1, 0, nullptr, 0, nullptr};
    static const wl_interface bad_utf8{"invalid-\xFF", 1, 0, nullptr, 0, nullptr};
    static const wl_interface zero{"invalid_zero", 1, 0, nullptr, 0, nullptr};
    const auto *invalid_interface = invalid == InvalidGlobal::empty ? &empty :
        invalid == InvalidGlobal::utf8 ? &bad_utf8 : invalid == InvalidGlobal::zero ? &zero : nullptr;
    if (invalid_interface && !expect(wl_global_create(display_, invalid_interface,
        invalid == InvalidGlobal::zero ? 0 : 1, nullptr, bind) != nullptr, "create malformed test announcement")) return;
    // Extra announcements share one interface description per name, whose
    // version is the highest announced for it.
    std::map<std::string, int> highest;
    for (const auto &[name, version] : extra) highest[name] = std::max(highest[name], version);
    std::map<std::string, const wl_interface *> described;
    for (const auto &[name, version] : highest) {
      names_.push_back(name);
      interfaces_.push_back(wl_interface{names_.back().c_str(), version, 0, nullptr, 0, nullptr});
      described.emplace(name, &interfaces_.back());
    }
    for (const auto &[name, version] : extra)
      if (!expect(wl_global_create(display_, described.at(name), version, nullptr, bind) != nullptr,
                  "create extra test announcement")) return;
    ready_ = true;
    thread_ = std::jthread([this](const std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (remove_output_.exchange(false) && output_) {
          wl_global_destroy(output_);
          output_ = nullptr;
        }
        (void)wl_event_loop_dispatch(wl_display_get_event_loop(display_), 10);
        wl_display_flush_clients(display_);
      }
    });
  }
  ~Server() { disconnect(); if (client_ >= 0) ::close(client_); }
  [[nodiscard]] int take_fd() noexcept { return ready_ ? std::exchange(client_, -1) : -1; }
  void remove_output() noexcept { remove_output_ = true; }
  void disconnect() {
    if (thread_.joinable()) { thread_.request_stop(); thread_.join(); }
    if (display_) {
      wl_display_destroy_clients(display_);
      wl_display_destroy(display_);
      display_ = nullptr;
    }
  }
private:
  wl_display *display_{};
  wl_global *output_{};
  int client_{-1};
  bool ready_{};
  std::atomic<bool> remove_output_{};
  std::deque<std::string> names_;
  std::deque<wl_interface> interfaces_;
  std::jthread thread_;
};

template <class Predicate> bool eventually(Predicate predicate) {
  const auto deadline = layerc::core::MonotonicClock::now() + std::chrono::seconds{2};
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  } while (layerc::core::MonotonicClock::now() < deadline);
  return false;
}

void registry_removal_and_loss() {
  Server server;
  const int fd = server.take_fd();
  if (!expect(fd >= 0, "create private Wayland server")) return;
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
  layerc::platform::WaylandConnection connection;
  if (!expect(connection.open() && connection.status().connected, "connect and complete registry handshake")) return;
  expect(connection.status().globals.at("wl_compositor") == static_cast<std::uint32_t>(Server::compositor_version()) &&
         connection.status().globals.at("wl_output") == static_cast<std::uint32_t>(Server::output_version()),
         "registry preserves advertised interface versions");
  expect(!connection.has_pending_events() && !connection.dispatch(), "idle transport creates no polling work");
  server.remove_output();
  expect(eventually([&] { (void)connection.dispatch(); return !connection.status().globals.contains("wl_output"); }),
         "global removal invalidates the registry snapshot");
  server.disconnect();
  expect(eventually([&] { (void)connection.dispatch(); return !connection.status().connected; }) &&
         connection.status().reason == "wayland_connection_lost" &&
         connection.fd() == -1 && !connection.raw(), "peer loss closes transport with a visible stable reason");
  expect(!connection.has_pending_events() && !connection.dispatch(), "connection loss is terminal without retry polling");
}
void malformed_registry_does_not_poison_status() {
  for (const auto invalid : {Server::InvalidGlobal::empty, Server::InvalidGlobal::utf8}) {
    Server server(invalid);
    const int fd = server.take_fd();
    if (!expect(fd >= 0, "create malformed private registry")) return;
    ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
    layerc::platform::WaylandConnection connection;
    expect(!connection.open() && connection.status().reason == "wayland_invalid_global" &&
           connection.status().globals.empty() && !connection.raw(),
           "malformed announcement rejects and releases its connection");
    layerc::runtime::RuntimeDiagnostics diagnostics;
    diagnostics.linux_extension.emplace();
    diagnostics.linux_extension->display.emplace();
    diagnostics.linux_extension->display->wayland = connection.status();
    const auto wire = layerc::runtime::serialize_runtime_diagnostics_json(diagnostics);
    const auto parsed = layerc::runtime::parse_runtime_diagnostics_json(wire);
    const auto *value = std::get_if<layerc::runtime::RuntimeDiagnostics>(&parsed);
    expect(value && value->linux_extension->display->wayland == connection.status(),
           "rejected globals retain a stable reason without poisoning typed status");
  }
}
void registry_bounds_are_explicit() {
  const auto open = [](const Server::Announcements &extra) {
    auto server = std::make_unique<Server>(Server::InvalidGlobal::none, extra);
    const int fd = server->take_fd();
    auto connection = std::make_unique<layerc::platform::WaylandConnection>();
    if (!expect(fd >= 0, "create bounded private registry")) return connection;
    ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
    (void)connection->open(layerc::core::Milliseconds{2000});
    return connection;
  };
  // Repeated instances past the old 128-instance cutoff still contribute the
  // highest advertised version of their interface.
  Server::Announcements repeated(200, {"layerc_test_repeated", 1});
  repeated.back().second = 9;
  {
    const auto connection = open(repeated);
    expect(connection->status().connected && connection->status().reason.empty() &&
           connection->status().globals.contains("layerc_test_repeated") &&
           connection->status().globals.at("layerc_test_repeated") == 9,
           "many instances of one interface keep the highest late version");
  }
  Server::Announcements distinct;
  for (int index = 0; index < 129; ++index) distinct.emplace_back("layerc_test_" + std::to_string(index), 1);
  const Server::Announcements instances(1100, {"layerc_test_repeated", 1});
  const std::array<const Server::Announcements *, 2> overflowing{&distinct, &instances};
  for (const auto *extra : overflowing) {
    const auto connection = open(*extra);
    expect(!connection->status().connected &&
           connection->status().reason == "wayland_too_many_globals" &&
           connection->status().globals.empty() && !connection->raw(),
           "exceeding a registry bound is a visible fault, not a truncated snapshot");
  }
}
void malformed_wire_announcement(const bool send_sync) {
  // libwayland-server rejects version 0 itself, so send this deliberately
  // malformed event over a private raw protocol peer instead.
  std::array<int, 2> sockets{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0,
              "create private zero-version peer")) return;
  const auto child = ::fork();
  if (child == 0) {
    ::close(sockets[0]);
    ::alarm(3); // Even an unexpectedly stalled client cannot strand waitpid.
    std::array<std::uint32_t, 6> requests{};
    if (::recv(sockets[1], requests.data(), sizeof(requests), MSG_WAITALL) !=
        static_cast<ssize_t>(sizeof(requests))) ::_exit(1);
    std::array<std::uint32_t, 12> reply{};
    reply[0] = requests[2]; // registry id from get_registry
    reply[1] = 36u << 16u; // global event: header, id, string and version
    reply[2] = 1;
    reply[3] = sizeof("invalid_zero");
    std::memcpy(reply.data() + 4, "invalid_zero", sizeof("invalid_zero"));
    reply[8] = 0;
    reply[9] = requests[5]; // callback id from sync
    reply[10] = 12u << 16u;
    reply[11] = 1;
    const auto bytes = send_sync ? sizeof(reply) : std::size_t{36};
    const auto sent = ::send(sockets[1], reply.data(), bytes, MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(bytes)) ::_exit(1);
    // Keep the peer alive until rejection closes the client. Otherwise HUP
    // can race the malformed announcement and hide its intended reason.
    const auto deadline = layerc::core::MonotonicClock::now() + std::chrono::seconds{2};
    for (;;) {
      const auto remaining = std::chrono::duration_cast<layerc::core::Milliseconds>(
          deadline - layerc::core::MonotonicClock::now()).count();
      if (remaining <= 0) ::_exit(2);
      pollfd watch{sockets[1], POLLIN, 0};
      const int ready = ::poll(&watch, 1, static_cast<int>(remaining));
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || (watch.revents & POLLNVAL) != 0) ::_exit(2);
      if ((watch.revents & (POLLHUP | POLLERR)) != 0) ::_exit(0);
      char discarded{};
      const auto received = ::recv(sockets[1], &discarded, 1, MSG_DONTWAIT);
      if (received == 0) ::_exit(0);
      if (received < 0 && errno != EINTR && errno != EAGAIN) ::_exit(2);
    }
  }
  ::close(sockets[1]);
  if (!expect(child > 0, "fork disposable malformed protocol peer")) { ::close(sockets[0]); return; }
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
  layerc::platform::WaylandConnection connection;
  const auto start = layerc::core::MonotonicClock::now();
  expect(!connection.open(layerc::core::Milliseconds{1000}) &&
         connection.status().reason == "wayland_invalid_global" &&
         connection.status().globals.empty() && !connection.raw() &&
         layerc::core::MonotonicClock::now() - start < std::chrono::milliseconds{500},
         "malformed registry rejects immediately, with or without a sync reply");
  int status{};
  expect(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "malformed peer is joined after delivering its bounded fixture");
}
void zero_version_wire_announcement() { malformed_wire_announcement(true); }
void zero_version_without_sync() { malformed_wire_announcement(false); }
void seam_connection_lifecycle() {
  Server server;
  const int fd = server.take_fd();
  if (!expect(fd >= 0, "create private display seam server")) return;
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
  ScopedEnvironment no_x("DISPLAY", std::nullopt);
  ScopedEnvironment desktop("XDG_CURRENT_DESKTOP", "");
  ScopedEnvironment backend("LAYERC_DISPLAY_BACKEND", "wayland");
  layerc::daemon::DisplayAdapters adapters;
  layerc::runtime::DaemonKernel kernel(adapters.runtime_services());
  layerc::platform::EventLoop loop;
  expect(!loop.poll_once(0).any(), "empty event loop is initially idle");
  layerc::runtime::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  adapters.start(loop, status, diagnostics);
  adapters.refresh({}, true, kernel, status, diagnostics);
  expect(status.display && status.display->session == layerc::runtime::DisplaySession::wayland &&
         status.display->compositor == layerc::runtime::DisplayCompositor::wlroots &&
         !loop.poll_once(0).any(), "seam detects paired wlroots globals and the idle connection does not wake");
  expect(status.display->providers.overlay.state == layerc::runtime::DisplayProviderState::not_built,
         "transport alone does not claim native overlay support");
  server.disconnect();
  expect(eventually([&] {
    const auto result = adapters.dispatch({});
    if (result.windows_changed) adapters.refresh({}, false, kernel, status, diagnostics);
    return status.display && status.display->session == layerc::runtime::DisplaySession::none;
  }), "explicit Wayland loses its session rather than switching to another backend");
  expect(!loop.poll_once(0).any() && !adapters.has_pending_events(),
         "lost fd is unregistered before blocking with no stale-ready busy loop");
  kernel.quiesce();
}
#ifdef LAYERC_TEST_XWAYLAND_LOSS
void forced_loss_drains_xwayland_queue() {
  layerc::test::require_x11();
  Server server;
  const int fd = server.take_fd();
  if (!expect(fd >= 0, "create private Wayland server over disposable X11")) return;
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(fd));
  ScopedEnvironment backend("LAYERC_DISPLAY_BACKEND", "wayland");
  layerc::daemon::DisplayAdapters adapters;
  layerc::runtime::DaemonKernel kernel(adapters.runtime_services());
  layerc::platform::EventLoop loop;
  layerc::runtime::LinuxStatusSnapshot status;
  std::vector<std::string> diagnostics;
  adapters.start(loop, status, diagnostics);
  adapters.refresh({}, true, kernel, status, diagnostics);
  if (!expect(status.x11.connected && status.display &&
              status.display->session == layerc::runtime::DisplaySession::wayland,
              "forced Wayland has a degraded XWayland connection")) {
    kernel.quiesce(); return;
  }
  server.disconnect();
  expect(eventually([&] {
    (void)loop.poll_once(0);
    const auto result = adapters.dispatch(loop.ready_fds());
    if (result.windows_changed) adapters.refresh({}, false, kernel, status, diagnostics);
    return status.display->session == layerc::runtime::DisplaySession::none;
  }), "Wayland loss never selects the live X11 session under an explicit override");

  std::string error;
  auto writer = layerc::platform::X11Display::open(error);
  if (!expect(writer.has_value(), "open disposable X11 property writer")) { kernel.quiesce(); return; }
  auto *raw = writer->raw();
  const auto property = XInternAtom(raw, "_LAYERC_TEST_LOSS_QUEUE", False);
  const unsigned long value = 1;
  XChangeProperty(raw, static_cast<::Window>(writer->root()), property, XA_CARDINAL,
                  32, PropModeReplace, reinterpret_cast<const unsigned char *>(&value), 1);
  XSync(raw, False);
  expect(eventually([&] { return adapters.has_pending_events(); }),
         "root event is buffered even though the selected session is now headless");
  (void)adapters.dispatch_pending();
  expect(!adapters.has_pending_events() && !loop.poll_once(0).any() &&
         adapters.session_count() == 0,
         "unselected XWayland is drained without retaining sessions or busy-loop work");
  XDeleteProperty(raw, static_cast<::Window>(writer->root()), property);
  XSync(raw, False);
  (void)loop.poll_once(0);
  (void)adapters.dispatch(loop.ready_fds());
  (void)adapters.dispatch_pending();
  kernel.quiesce();
}
#endif
} // namespace
#ifdef LAYERC_TEST_XWAYLAND_LOSS
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(registry_removal_and_loss),
                 LAYERC_TEST_CASE(malformed_registry_does_not_poison_status),
                 LAYERC_TEST_CASE(registry_bounds_are_explicit),
                 LAYERC_TEST_CASE(zero_version_wire_announcement),
                 LAYERC_TEST_CASE(zero_version_without_sync),
                 LAYERC_TEST_CASE(seam_connection_lifecycle),
                 LAYERC_TEST_CASE(forced_loss_drains_xwayland_queue))
#else
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(registry_removal_and_loss),
                 LAYERC_TEST_CASE(malformed_registry_does_not_poison_status),
                 LAYERC_TEST_CASE(registry_bounds_are_explicit),
                 LAYERC_TEST_CASE(zero_version_wire_announcement),
                 LAYERC_TEST_CASE(zero_version_without_sync),
                 LAYERC_TEST_CASE(seam_connection_lifecycle))
#endif
