#include "a11y/atspi/loop_bus.hpp"

#include <systemd/sd-bus.h>

#include <FL/Fl.H>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <poll.h>

namespace layerc::editor::a11y::atspi {

LoopBus::LoopBus(sd_bus *bus) : bus_(bus) {}

LoopBus::~LoopBus() {
  timeout_.cancel();
  if (!bus_) return;
  release_fd();
  if (continue_armed_) {
    Fl::remove_timeout(on_continue, this);
    continue_armed_ = false;
  }
  // Never flush: sd_bus_flush polls with an infinite timeout, so a congested or
  // hung peer would freeze the editor at exit. Queued messages are best effort,
  // and the registry also drops us when our name disappears
  // (registryd_registry.c:386).
  sd_bus_close_unref(bus_);
  bus_ = nullptr;
}

void LoopBus::release_fd() {
  if (fd_ >= 0) Fl::remove_fd(fd_);
  fd_ = -1;
  read_registered_ = false;
  write_registered_ = false;
}

bool LoopBus::watch_fd() {
  if (!bus_) return false;
  const int fd = sd_bus_get_fd(bus_);
  if (fd < 0) {
    fail("the bus connection has no file descriptor");
    return false;
  }
  if (fd != fd_) {
    // A connection keeps one socket, so this only happens if the number was
    // never watched: never leave the old number in Fl's watch list.
    release_fd();
    fd_ = fd;
  }
  // Readable notifications are watched for the connection's whole life: they
  // carry messages the loop has not asked for yet.
  if (!read_registered_) {
    Fl::add_fd(fd, FL_READ, on_fd, this);
    read_registered_ = true;
  }
  return true;
}

void LoopBus::fail(std::string why) {
  if (failure_.empty()) failure_ = std::move(why);
  release_fd();
  timeout_.cancel();
  if (continue_armed_) {
    Fl::remove_timeout(on_continue, this);
    continue_armed_ = false;
  }
  // Inside dispatch: the owner only schedules its own teardown.
  if (!reported_ && failure_handler_) {
    reported_ = true;
    failure_handler_(failure_handler_data_);
  }
}

bool LoopBus::attach() {
  if (!watch_fd()) return false;
  dispatch();
  return !failed();
}

void LoopBus::dispatch() {
  if (!bus_ || failed()) return;
  // A peer that floods the socket must not starve the rest of the FLTK loop:
  // the bounded drain returns to Fl::wait and the continuation timer resumes on
  // the next iteration. The continuation has its own callback, so schedule()
  // never removes it while it re-arms the sd-bus deadline below.
  int processed = 0;
  for (int steps = 0; steps < kMaxMessagesPerDispatch; ++steps) {
    processed = sd_bus_process(bus_, nullptr);
    if (processed <= 0) break;
  }
  if (processed < 0) {
    fail(std::string{"the bus connection failed: "} + std::strerror(-processed));
    return;
  }
  if (processed > 0 && !continue_armed_) {
    Fl::add_timeout(0.0, on_continue, this);
    continue_armed_ = true;
  }
  schedule();
}

void LoopBus::schedule() {
  if (!bus_ || failed()) return;
  if (!watch_fd()) return;
  const int events = sd_bus_get_events(bus_);
  const bool writable = events >= 0 && (events & POLLOUT) != 0;
  if (writable != write_registered_) {
    if (writable)
      Fl::add_fd(fd_, FL_WRITE, on_fd, this);
    else
      Fl::remove_fd(fd_, FL_WRITE);
    write_registered_ = writable;
  }
  std::uint64_t deadline = 0;
  if (sd_bus_get_timeout(bus_, &deadline) < 0 || deadline == UINT64_MAX) {
    timeout_.cancel();
    return;
  }
  // sd-bus reports an absolute CLOCK_MONOTONIC deadline in microseconds; the
  // steady_clock epoch is unspecified, so the remainder uses that clock itself.
  timespec monotonic{};
  clock_gettime(CLOCK_MONOTONIC, &monotonic);
  const auto elapsed = static_cast<std::uint64_t>(monotonic.tv_sec) * 1000000u +
                       static_cast<std::uint64_t>(monotonic.tv_nsec) / 1000u;
  const auto remaining = deadline > elapsed ? deadline - elapsed : 0u;
  using Clock = SteadyTimeout::Clock;
  using namespace std::chrono;
  // Bound the conversion and leave room for the clock reading in start().
  const auto limit = static_cast<std::uint64_t>(
      duration_cast<microseconds>((Clock::time_point::max() - Clock::now()) / 2).count());
  timeout_.start(microseconds(std::min(remaining, limit)));
}

void LoopBus::on_fd(int, void *data) { static_cast<LoopBus *>(data)->dispatch(); }

void LoopBus::on_continue(void *data) {
  auto *loop = static_cast<LoopBus *>(data);
  loop->continue_armed_ = false;
  loop->dispatch();
}

bool send_message(LoopBus &loop, sd_bus_message *message) {
  if (!message) return false;
  const int sent = sd_bus_send(loop.bus(), message, nullptr);
  sd_bus_message_unref(message);
  loop.schedule();
  return sent >= 0;
}

bool call_async(LoopBus &loop, sd_bus_message *message,
                const std::uint64_t timeout_usec, ReplyHandler reply,
                void *userdata, sd_bus_slot **slot) {
  if (!message) return false;
  const int started = sd_bus_call_async(loop.bus(), slot, message, reply,
                                        userdata, timeout_usec);
  sd_bus_message_unref(message);
  loop.schedule();
  return started >= 0;
}

} // namespace layerc::editor::a11y::atspi
