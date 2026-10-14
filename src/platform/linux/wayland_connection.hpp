#pragma once

#include "layerc/core/types.hpp"
#include "layerc/runtime/display_diagnostics.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct wl_display;
struct wl_registry;

namespace layerc::platform {

// One advertised registry global; `name` is the numeric registry id that
// wl_registry_bind needs.
struct WaylandGlobal {
  std::uint32_t name{};
  std::string interface;
  std::uint32_t version{};

  bool operator==(const WaylandGlobal &) const = default;
};

// One owned connection on the daemon thread. No adapter opens a second
// connection or starts a worker. The initial registry handshake is bounded;
// after that, all reads/flushes are nonblocking through EventLoop.
class WaylandConnection {
public:
  WaylandConnection();
  ~WaylandConnection();
  WaylandConnection(const WaylandConnection &) = delete;
  WaylandConnection &operator=(const WaylandConnection &) = delete;

  [[nodiscard]] bool open(core::Milliseconds timeout = core::Milliseconds{250});
  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] short poll_events() const noexcept;
  // Drains callbacks already buffered by libwayland before the host sleeps.
  [[nodiscard]] bool has_pending_events();
  // Reads only when immediately readable. Returns true for registry/loss
  // changes; losing the connection is terminal, with no polling retry loop.
  [[nodiscard]] bool dispatch();
  [[nodiscard]] const runtime::WaylandStatusSnapshot &status() const noexcept;
  [[nodiscard]] wl_display *raw() const noexcept;
  [[nodiscard]] wl_registry *registry() const noexcept;
  // Current registry instances, ordered by id; empty while disconnected.
  [[nodiscard]] std::vector<WaylandGlobal> globals() const;
  // Called before the display is disconnected (loss, reopen or destruction)
  // so providers destroy their proxies while it is still valid.
  void set_release_hook(std::function<void()> hook);
  // Sends queued requests without blocking; a full socket arms POLLOUT.
  void flush();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::platform
