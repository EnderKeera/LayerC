#include "wayland_connection.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <thread>
#include <vector>
#include <wayland-client.h>

// Test-only interposition on generated display new-id requests. Both requests
// have exactly one null new-id argument; forwarding through the array API
// exercises the real transport without linker flags or product injection.
namespace {
enum class AllocationFailure { none, registry, sync };
AllocationFailure allocation_failure{};
}
extern "C" wl_proxy *wl_proxy_marshal_flags(wl_proxy *proxy, std::uint32_t opcode,
    const wl_interface *interface, std::uint32_t version, std::uint32_t flags, ...) {
  if ((allocation_failure == AllocationFailure::registry && interface == &wl_registry_interface) ||
      (allocation_failure == AllocationFailure::sync && interface == &wl_callback_interface))
    return nullptr;
  wl_argument argument{};
  return wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, &argument);
}

namespace {
using layerc::test::expect;
using layerc::test::ScopedEnvironment;
using layerc::platform::WaylandConnection;

// The listening backlog is private, never an actual compositor or user socket.
class Listener {
public:
  explicit Listener(const int backlog) {
    address_.sun_family = AF_UNIX;
    const auto path = (directory_.path() / "display").string();
    if (path.size() >= sizeof(address_.sun_path)) return;
    std::memcpy(address_.sun_path, path.c_str(), path.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    ready_ = fd_ >= 0 &&
        ::bind(fd_, reinterpret_cast<const sockaddr *>(&address_), sizeof(address_)) == 0 &&
        ::listen(fd_, backlog) == 0;
  }
  ~Listener() {
    for (const int fd : queued_) ::close(fd);
    if (fd_ >= 0) ::close(fd_);
  }
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] const std::filesystem::path &directory() const noexcept { return directory_.path(); }
  [[nodiscard]] const char *path() const noexcept { return address_.sun_path; }
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] bool fill() {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (client < 0) return false;
      const int result = ::connect(client, reinterpret_cast<const sockaddr *>(&address_), sizeof(address_));
      if (result == 0) { queued_.push_back(client); continue; }
      const int error = errno;
      ::close(client);
      return error == EAGAIN;
    }
    return false;
  }
private:
  layerc::test::TempDirectory directory_{"layerc-wayland-connect"};
  sockaddr_un address_{};
  int fd_{-1};
  bool ready_{};
  std::vector<int> queued_;
};

void named_socket_uses_same_deadline() {
  for (const bool absolute : {false, true}) {
    Listener listener(1);
    if (!expect(listener.ready(), "create private named Wayland listener")) return;
    ScopedEnvironment socket("WAYLAND_SOCKET", std::nullopt);
    ScopedEnvironment runtime("XDG_RUNTIME_DIR", listener.directory());
    ScopedEnvironment display("WAYLAND_DISPLAY", absolute ? listener.path() : "display");
    WaylandConnection connection;
    const auto start = layerc::core::MonotonicClock::now();
    expect(!connection.open(layerc::core::Milliseconds{30}) &&
           connection.status().reason == "wayland_handshake_timeout" &&
           layerc::core::MonotonicClock::now() - start < std::chrono::seconds{1},
           "relative and absolute sockets complete connect but bound a stalled handshake");
    const int accepted = ::accept4(listener.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (expect(accepted >= 0, "the private listener received the named connection")) {
      std::array<char, 64> pending{};
      while (::recv(accepted, pending.data(), pending.size(), MSG_DONTWAIT) > 0) {}
      expect(::recv(accepted, pending.data(), 1, MSG_DONTWAIT) == 0,
             "handshake timeout also closes the newly created named socket");
      ::close(accepted);
    }
    expect(!connection.raw() && !connection.registry() && connection.fd() < 0 &&
           !connection.has_pending_events(), "named timeout retains no owned transport or retry");
  }
}

void full_listening_backlog_is_bounded() {
  Listener listener(0);
  if (!expect(listener.ready() && listener.fill(), "exhaust a private listening backlog")) return;
  ScopedEnvironment socket("WAYLAND_SOCKET", std::nullopt);
  ScopedEnvironment display("WAYLAND_DISPLAY", listener.path());
  const auto child = ::fork();
  if (child == 0) {
    WaylandConnection connection;
    const auto start = layerc::core::MonotonicClock::now();
    const bool opened = connection.open(layerc::core::Milliseconds{50});
    const bool bounded = !opened && connection.status().reason == "cannot_open_wayland" &&
        !connection.raw() && connection.fd() < 0 &&
        layerc::core::MonotonicClock::now() - start < std::chrono::milliseconds{500};
    ::_exit(bounded ? 0 : 1);
  }
  if (!expect(child > 0, "fork owned backlog regression probe")) return;
  const auto deadline = layerc::core::MonotonicClock::now() + std::chrono::seconds{2};
  int status{};
  pid_t joined{};
  do {
    joined = ::waitpid(child, &status, WNOHANG);
    if (joined != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  } while (layerc::core::MonotonicClock::now() < deadline);
  if (joined == 0) {
    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, &status, 0);
  }
  expect(joined == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "full backlog cannot block daemon startup before the registry timeout");
}

void absent_display() {
  ScopedEnvironment display("WAYLAND_DISPLAY", std::nullopt);
  ScopedEnvironment socket("WAYLAND_SOCKET", std::nullopt);
  WaylandConnection connection;
  expect(!connection.open() && connection.status().reason == "no_wayland_display" &&
         connection.fd() < 0 && !connection.raw(),
         "missing environment never probes an unintended default compositor");
  expect(!connection.has_pending_events() && !connection.dispatch(), "absent connection has no retry work");
}
void bounded_handshake_and_cleanup() {
  std::array<int, 2> sockets{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0,
              "create private stalled compositor socket")) return;
  ScopedEnvironment display("WAYLAND_DISPLAY", std::nullopt);
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
  WaylandConnection connection;
  const auto start = layerc::core::MonotonicClock::now();
  expect(!connection.open(layerc::core::Milliseconds{30}) &&
         connection.status().reason == "wayland_handshake_timeout",
         "stalled registry handshake has a monotonic bounded deadline");
  expect(layerc::core::MonotonicClock::now() - start < std::chrono::seconds{1} &&
         connection.fd() < 0 && !connection.raw() &&
         ::fcntl(sockets[0], F_GETFD) == -1,
         "failed handshake releases connection ownership before returning");
  ::close(sockets[1]);
  expect(!connection.has_pending_events() && !connection.dispatch(), "timeout does not create a retry loop");
}
void default_handshake_deadline() {
  std::array<int, 2> sockets{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0,
              "create private default-timeout socket")) return;
  ScopedEnvironment display("WAYLAND_DISPLAY", std::nullopt);
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
  WaylandConnection connection;
  const auto start = layerc::core::MonotonicClock::now();
  const bool opened = connection.open();
  const auto elapsed = layerc::core::MonotonicClock::now() - start;
  expect(!opened && connection.status().reason == "wayland_handshake_timeout" &&
         elapsed >= std::chrono::milliseconds{200} && elapsed < std::chrono::milliseconds{800},
         "production default is a quarter-second, not a one-second handshake");
  expect(!connection.raw() && !connection.registry() && connection.fd() < 0 &&
         ::fcntl(sockets[0], F_GETFD) == -1, "default timeout releases owned transport objects");
  ::close(sockets[1]);
}
void allocation_failures_release_transport() {
  for (const auto failure : {AllocationFailure::registry, AllocationFailure::sync}) {
    std::array<int, 2> sockets{-1, -1};
    if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0,
                "create private allocation-failure socket")) return;
    ScopedEnvironment display("WAYLAND_DISPLAY", std::nullopt);
    ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
    WaylandConnection connection;
    allocation_failure = failure;
    const bool opened = connection.open();
    allocation_failure = AllocationFailure::none;
    expect(!opened && connection.status().reason == "wayland_registry_failed" &&
           !connection.raw() && !connection.registry() && connection.fd() < 0 &&
           ::fcntl(sockets[0], F_GETFD) == -1,
           "registry/sync allocation failure closes the connection and every proxy");
    // Early failures publish one diagnostic transition; acknowledging it
    // must leave no timer, connection, or repeated pending work.
    expect(connection.dispatch() && !connection.has_pending_events() && !connection.dispatch(),
           "allocation failure publishes once without retry work");
    ::close(sockets[1]);
  }
}
void disconnected_peer() {
  std::array<int, 2> sockets{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0,
              "create private closed compositor socket")) return;
  ::close(sockets[1]);
  ScopedEnvironment display("WAYLAND_DISPLAY", std::nullopt);
  ScopedEnvironment socket("WAYLAND_SOCKET", std::to_string(sockets[0]));
  WaylandConnection connection;
  expect(!connection.open(layerc::core::Milliseconds{30}) &&
         connection.status().reason == "wayland_connection_lost" &&
         connection.fd() < 0 && !connection.raw(),
         "peer loss retains a typed reason and releases the failed transport");
}
} // namespace
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(absent_display),
                 LAYERC_TEST_CASE(named_socket_uses_same_deadline),
                 LAYERC_TEST_CASE(full_listening_backlog_is_bounded),
                 LAYERC_TEST_CASE(bounded_handshake_and_cleanup),
                 LAYERC_TEST_CASE(default_handshake_deadline),
                 LAYERC_TEST_CASE(allocation_failures_release_transport),
                 LAYERC_TEST_CASE(disconnected_peer))
