#pragma once

// The daemon's one session-bus connection (docs/Wayland.md, "Seam"). Opened
// only when a provider needs it; every call is asynchronous and the socket
// and sd-bus timeouts are driven by the daemon's single EventLoop, so no
// thread and no blocking call exists here.

#include "event_loop.hpp"

#include "layerc/core/types.hpp"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {

struct BusSlotRelease {
  void operator()(sd_bus_slot *slot) const noexcept { sd_bus_slot_unref(slot); }
};
using BusSlot = std::unique_ptr<sd_bus_slot, BusSlotRelease>;

// Clients own a guard; replies and signals whose guard is gone are dropped,
// so a destroyed client is never called back. BusConnection::release() also
// cancels the guard's pending calls, freeing their captures and timeouts.
// Messages already queued are still written: cancellation never retracts a
// remote operation, so clients keep their explicit close/release protocol.
using BusGuard = std::shared_ptr<int>;
[[nodiscard]] inline BusGuard make_bus_guard() { return std::make_shared<int>(0); }

using BusAppend = std::function<int(sd_bus_message *)>;
// `error` is null on success; `reply` is the method return or error message.
using BusReply = std::function<void(sd_bus_message *reply, const sd_bus_error *error)>;
using BusSignal = std::function<void(sd_bus_message *signal)>;

inline constexpr std::uint64_t bus_call_timeout_usec = 5'000'000;

class BusConnection {
public:
  BusConnection() = default;
  ~BusConnection();
  BusConnection(const BusConnection &) = delete;
  BusConnection &operator=(const BusConnection &) = delete;

  // Connects to $DBUS_SESSION_BUS_ADDRESS (or the user bus) and registers
  // the socket on `loop`. A failure keeps the connection closed with reason
  // `bus_unavailable`; there is no retry timer.
  [[nodiscard]] bool open(EventLoop &loop);
  void close() noexcept;
  [[nodiscard]] bool connected() const noexcept { return bus_ != nullptr; }
  // Empty while connected or never opened; otherwise a stable token.
  [[nodiscard]] const std::string &reason() const noexcept { return reason_; }
  [[nodiscard]] sd_bus *raw() const noexcept { return bus_; }
  [[nodiscard]] std::string unique_name() const;

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] bool outgoing_pending() const noexcept;
  // When sd-bus needs processing without socket readiness (queued messages,
  // call timeouts). nullopt: wait for the socket only.
  [[nodiscard]] std::optional<core::TimePoint> next_deadline() const noexcept;
  // Processes everything queued. true when any message was handled. A lost
  // connection is closed with reason `bus_connection_lost`.
  bool dispatch();

  // Asynchronous method call; false when the message could not be queued.
  bool call(const char *destination, const char *path, const char *interface,
            const char *member, const BusAppend &append, BusReply reply,
            const BusGuard &guard);
  // org.freedesktop.DBus.Properties.Get; `reply` receives the variant-bearing
  // reply positioned at its single argument.
  bool get_property(const char *destination, const char *path, const char *interface,
                    const char *property, BusReply reply, const BusGuard &guard);
  // A signal match owned by the returned slot.
  [[nodiscard]] BusSlot add_match(const std::string &rule, BusSignal handler,
                                  const BusGuard &guard);
  // Releases a slot after the current dispatch finishes, so handlers may drop
  // their own match.
  void defer_release(BusSlot slot);
  // Resets `guard` and cancels every pending call it guarded.
  void release(BusGuard &guard) noexcept;
  [[nodiscard]] std::size_t pending_calls() const noexcept { return pending_.size(); }

  // Tracks the current unique-name owner of `name` (GetNameOwner plus
  // NameOwnerChanged); nullopt until known or while unowned.
  void watch_name(const std::string &name);
  [[nodiscard]] std::optional<std::string> owner(std::string_view name) const;
  // Whether an initial GetNameOwner reply has arrived for `name`.
  [[nodiscard]] bool owner_known(std::string_view name) const;
  // True once after any watched owner changed or became known.
  [[nodiscard]] bool take_owners_changed() noexcept;

private:
  struct Watch {
    std::optional<std::string> owner;
    bool known{};
    BusSlot match;
  };
  // An owned reply slot; dropping it cancels the call and its timeout.
  struct PendingCall {
    BusSlot slot;
    std::weak_ptr<int> guard;
    std::shared_ptr<const bool> done;
  };

  // Drops answered calls and calls whose guard is gone. During dispatch the
  // slots are released after it finishes, never inside their own callback.
  void prune_calls() noexcept;
  void sync_registration() noexcept;
  void lost() noexcept;
  void set_owner(const std::string &name, std::optional<std::string> owner);

  sd_bus *bus_{};
  EventLoop *loop_{};
  int fd_{-1};
  short events_{};
  std::string reason_;
  bool owners_changed_{};
  bool dispatching_{};
  BusGuard guard_;
  std::map<std::string, Watch, std::less<>> watches_;
  std::vector<PendingCall> pending_;
  std::vector<BusSlot> released_;
};

// The handle path a portal Request uses for `token` on this connection.
[[nodiscard]] std::string portal_request_path(std::string_view unique_name,
                                              std::string_view token);

} // namespace layerc::platform
