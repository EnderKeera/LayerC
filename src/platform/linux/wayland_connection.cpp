#include "wayland_connection.hpp"

#include "layerc/core/utf8.hpp"

#include <wayland-client.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {
[[nodiscard]] wl_display *connect_display(const core::TimePoint deadline) {
  // Libwayland consumes an inherited WAYLAND_SOCKET without connecting; keep
  // its one-shot fd ownership and environment semantics unchanged.
  const auto *inherited = std::getenv("WAYLAND_SOCKET");
  if (inherited && *inherited) return wl_display_connect(nullptr);

  const auto *name = std::getenv("WAYLAND_DISPLAY");
  if (!name || !*name) return nullptr;
  std::string path{name};
  if (path.front() != '/') {
    const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return nullptr;
    path = std::string(runtime) + "/" + path;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path) ||
      core::MonotonicClock::now() >= deadline) return nullptr;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return nullptr;
  const int result = ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  if (result < 0) {
    // AF_UNIX returns EAGAIN for a full listen queue without starting a
    // connection. Do not spin/retry on a socket that remains writable.
    if (errno != EINPROGRESS) { ::close(fd); return nullptr; }
    for (;;) {
      const auto remaining = std::chrono::duration_cast<core::Milliseconds>(
          deadline - core::MonotonicClock::now()).count();
      if (remaining <= 0) { ::close(fd); return nullptr; }
      pollfd watch{fd, POLLOUT, 0};
      const int ready = ::poll(&watch, 1, static_cast<int>(
          std::min<std::int64_t>(remaining, INT_MAX)));
      if (ready < 0 && errno == EINTR) continue;
      int error{};
      socklen_t size = sizeof(error);
      if (ready <= 0 || (watch.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
          ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
        ::close(fd);
        return nullptr;
      }
      break;
    }
  }
  // wl_display_connect_to_fd owns and closes fd even on allocation failure.
  return wl_display_connect_to_fd(fd);
}
} // namespace

struct WaylandConnection::Impl {
  // The interface bound matches the status parser's wayland.globals limit.
  static constexpr std::size_t max_global_interfaces = 128;
  static constexpr std::size_t max_global_instances = 1024;
  wl_display *display{};
  wl_registry *registry{};
  runtime::WaylandStatusSnapshot status;
  std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> globals;
  bool changed{};
  bool blocked_write{};

  ~Impl() { disconnect(); }
  void disconnect() noexcept {
    if (registry) wl_registry_destroy(std::exchange(registry, nullptr));
    if (display) wl_display_disconnect(std::exchange(display, nullptr));
  }
  void fail(const char *reason) {
    status.connected = false;
    status.reason = reason;
    status.globals.clear();
    globals.clear();
    changed = true;
  }
  void rebuild() {
    status.globals.clear();
    for (const auto &[id, value] : globals) {
      (void)id;
      auto &version = status.globals[value.first];
      version = std::max(version, value.second);
    }
    changed = true;
  }
  static void global(void *data, wl_registry *, std::uint32_t id,
                     const char *interface, std::uint32_t version) {
    auto &self = *static_cast<Impl *>(data);
    // Match shared wire constraints; malformed globals must not poison status.
    if (!self.status.reason.empty()) return;
    const std::string_view name{interface};
    if (name.empty() || name.size() > 128 || !core::is_valid_utf8(name) || version == 0) {
      self.fail("wayland_invalid_global");
      return;
    }
    // Instances (one per output, seat, ...) and distinct interfaces have
    // separate bounds; exceeding either is a visible fault, never a silently
    // incomplete snapshot.
    if (self.globals.size() >= max_global_instances && !self.globals.contains(id)) {
      self.fail("wayland_too_many_globals");
      return;
    }
    self.globals.insert_or_assign(id, std::pair{std::string(name), version});
    self.rebuild();
    if (self.status.globals.size() > max_global_interfaces) self.fail("wayland_too_many_globals");
  }
  static void removed(void *data, wl_registry *, std::uint32_t id) {
    auto &self = *static_cast<Impl *>(data);
    if (self.globals.erase(id) > 0) self.rebuild();
  }
  bool flush() {
    if (wl_display_flush(display) >= 0) { blocked_write = false; return true; }
    if (errno == EAGAIN) { blocked_write = true; return true; }
    fail("wayland_connection_lost");
    return false;
  }
  bool pending() {
    if (wl_display_dispatch_pending(display) >= 0) return true;
    fail("wayland_connection_lost");
    return false;
  }
};

WaylandConnection::WaylandConnection() : impl_(std::make_unique<Impl>()) {}
WaylandConnection::~WaylandConnection() = default;

bool WaylandConnection::open(const core::Milliseconds timeout) {
  // Opening/reopening explicitly replaces the old owned connection.
  impl_ = std::make_unique<Impl>();
  auto &self = *impl_;
  const auto *name = std::getenv("WAYLAND_DISPLAY");
  const auto *socket = std::getenv("WAYLAND_SOCKET");
  if ((!name || !*name) && (!socket || !*socket)) {
    self.status.reason = "no_wayland_display";
    return false;
  }
  const auto deadline = core::MonotonicClock::now() + timeout;
  self.display = connect_display(deadline);
  if (!self.display) { self.status.reason = "cannot_open_wayland"; return false; }
  self.registry = wl_display_get_registry(self.display);
  if (!self.registry) { self.fail("wayland_registry_failed"); self.disconnect(); return false; }
  static const wl_registry_listener listener{Impl::global, Impl::removed};
  if (wl_registry_add_listener(self.registry, &listener, &self) != 0) {
    self.fail("wayland_registry_failed");
    self.disconnect();
    return false;
  }
  bool done{};
  auto *sync = wl_display_sync(self.display);
  if (!sync) { self.fail("wayland_registry_failed"); self.disconnect(); return false; }
  static const wl_callback_listener sync_listener{
      [](void *data, wl_callback *, std::uint32_t) { *static_cast<bool *>(data) = true; }};
  if (wl_callback_add_listener(sync, &sync_listener, &done) != 0) {
    wl_callback_destroy(sync);
    self.fail("wayland_registry_failed");
    self.disconnect();
    return false;
  }
  while (!done) {
    if (!self.pending() || !self.status.reason.empty()) break;
    if (done) break;
    if (wl_display_prepare_read(self.display) != 0) continue;
    if (!self.flush()) { wl_display_cancel_read(self.display); break; }
    const auto remaining = std::chrono::duration_cast<core::Milliseconds>(
        deadline - core::MonotonicClock::now()).count();
    if (remaining <= 0) {
      wl_display_cancel_read(self.display);
      self.fail("wayland_handshake_timeout");
      break;
    }
    pollfd watch{wl_display_get_fd(self.display), poll_events(), 0};
    const int result = ::poll(&watch, 1, static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
    if (result < 0 && errno == EINTR) { wl_display_cancel_read(self.display); continue; }
    if (result <= 0 || (watch.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      wl_display_cancel_read(self.display);
      self.fail(result == 0 ? "wayland_handshake_timeout" : "wayland_connection_lost");
      break;
    }
    if ((watch.revents & POLLIN) != 0) {
      if (wl_display_read_events(self.display) != 0) {
        self.fail("wayland_connection_lost");
        break;
      }
    } else {
      wl_display_cancel_read(self.display);
    }
  }
  wl_callback_destroy(sync);
  self.status.connected = done && self.status.reason.empty();
  self.changed = false;
  if (!self.status.connected) self.disconnect();
  return self.status.connected;
}
int WaylandConnection::fd() const noexcept {
  return impl_->status.connected ? wl_display_get_fd(impl_->display) : -1;
}
short WaylandConnection::poll_events() const noexcept {
  return static_cast<short>(POLLIN | (impl_->blocked_write ? POLLOUT : 0));
}
bool WaylandConnection::has_pending_events() {
  if (!impl_->status.connected) return impl_->changed;
  (void)impl_->pending();
  if (impl_->status.connected) (void)impl_->flush();
  if (!impl_->status.connected) impl_->disconnect();
  return impl_->changed;
}
bool WaylandConnection::dispatch() {
  auto &self = *impl_;
  if (self.status.connected && self.pending()) {
    if (wl_display_prepare_read(self.display) == 0) {
      if (self.flush()) {
        pollfd watch{fd(), poll_events(), 0};
        const int result = ::poll(&watch, 1, 0);
        if (result > 0 && (watch.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          wl_display_cancel_read(self.display);
          self.fail("wayland_connection_lost");
        } else if (result > 0 && (watch.revents & POLLIN) != 0) {
          if (wl_display_read_events(self.display) != 0) self.fail("wayland_connection_lost");
          else (void)self.pending();
        } else {
          wl_display_cancel_read(self.display);
        }
      } else {
        wl_display_cancel_read(self.display);
      }
    }
    if (self.status.connected) (void)self.flush();
  }
  if (!self.status.connected) self.disconnect();
  return std::exchange(self.changed, false);
}
const runtime::WaylandStatusSnapshot &WaylandConnection::status() const noexcept {
  return impl_->status;
}
wl_display *WaylandConnection::raw() const noexcept { return impl_->display; }
wl_registry *WaylandConnection::registry() const noexcept { return impl_->registry; }
} // namespace layerc::platform
