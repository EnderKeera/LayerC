#pragma once

// Disposable dbus-run-session fixtures. Synchronous setup is test-only;
// product methods remain async. No fixture starts a worker thread.
#include "dbus_connection.hpp"
#include "test_support.hpp"

#include <poll.h>
#include <systemd/sd-bus.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace layerc::test::dbus {

struct BusRelease {
  void operator()(sd_bus *bus) const noexcept { sd_bus_flush_close_unref(bus); }
};
using Peer = std::unique_ptr<sd_bus, BusRelease>;

inline Peer peer(const char *name = nullptr) {
  sd_bus *bus{};
  if (!expect(sd_bus_open_user(&bus) >= 0, "test bus connects")) return {};
  Peer result{bus};
  if (name) expect(sd_bus_request_name(bus, name, 0) >= 0, "fake service claims its name");
  return result;
}

inline std::string unique(sd_bus *bus) {
  const char *name{};
  return sd_bus_get_unique_name(bus, &name) >= 0 && name ? name : "";
}

inline bool pump(platform::BusConnection &client, const std::vector<sd_bus *> &peers,
                 const std::function<bool()> &done, int timeout_ms = 2000) {
  const auto end = core::MonotonicClock::now() + std::chrono::milliseconds{timeout_ms};
  do {
    for (auto *bus : peers)
      for (int budget = 0; bus && budget < 1024 && sd_bus_process(bus, nullptr) > 0; ++budget) {}
    client.dispatch();
    if (done()) return true;
    std::vector<pollfd> fds;
    if (client.raw())
      fds.push_back({client.fd(), static_cast<short>(sd_bus_get_events(client.raw())), 0});
    for (auto *bus : peers)
      if (bus) fds.push_back({sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0});
    (void)::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2);
  } while (core::MonotonicClock::now() < end);
  return done();
}

// Cancelled calls keep no reply timeout, but a peer's late reply is still
// delivered once and dropped. Pump briefly so idle checks see only a
// quiescent connection: no deadline and no unwritten message.
inline bool settle(platform::BusConnection &client, const std::vector<sd_bus *> &peers) {
  (void)pump(client, peers, [] { return false; }, 100);
  return !client.next_deadline() && !client.outgoing_pending();
}

inline platform::BusSlot method(sd_bus *bus, const char *path, const char *interface,
                                const sd_bus_vtable *vtable, void *userdata) {
  sd_bus_slot *slot{};
  expect(sd_bus_add_object_vtable(bus, &slot, path, interface, vtable, userdata) >= 0,
         "fake service exports its object");
  return platform::BusSlot{slot};
}

struct Result {
  bool done{};
  std::string error;
};

inline platform::BusSlot call(sd_bus *bus, const char *name, const char *path,
                              const char *interface, const char *member,
                              const platform::BusAppend &append, Result &result) {
  sd_bus_message *message{};
  expect(sd_bus_message_new_method_call(bus, &message, name, path, interface, member) >= 0,
         "test call allocates");
  if (append) expect(append(message) >= 0, "test call appends");
  sd_bus_slot *slot{};
  expect(sd_bus_call_async(bus, &slot, message,
      [](sd_bus_message *reply, void *userdata, sd_bus_error *) {
        auto &out = *static_cast<Result *>(userdata);
        const auto *error = sd_bus_message_get_error(reply);
        out.error = error && error->name ? error->name : "";
        out.done = true;
        return 0;
      }, &result, 1'000'000) >= 0, "test call queues");
  sd_bus_message_unref(message);
  return platform::BusSlot{slot};
}

} // namespace layerc::test::dbus
