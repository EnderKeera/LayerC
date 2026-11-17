#include "a11y/atspi/loop_bus.hpp"

#include <systemd/sd-bus.h>

#include <FL/Fl.H>

#include <chrono>
#include <cstring>
#include <ctime>
#include <poll.h>

namespace layerc::editor::a11y::atspi {

LoopBus::LoopBus(sd_bus *bus) : bus_(bus) {}

LoopBus::~LoopBus() {
  if (!bus_) return;
  release_fd();
  if (timeout_armed_) {
    Fl::remove_timeout(on_timeout, this);
    timeout_armed_ = false;
  }
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
  if (timeout_armed_) {
    Fl::remove_timeout(on_timeout, this);
    timeout_armed_ = false;
  }
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
  double delay = -1.0;
  if (sd_bus_get_timeout(bus_, &deadline) >= 0 && deadline != UINT64_MAX) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const auto microseconds = static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
                              static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
    delay = deadline > microseconds
                ? static_cast<double>(deadline - microseconds) / 1.0e6
                : 0.0;
  }
  if (timeout_armed_) {
    Fl::remove_timeout(on_timeout, this);
    timeout_armed_ = false;
  }
  if (delay >= 0.0) {
    Fl::add_timeout(delay, on_timeout, this);
    timeout_armed_ = true;
  }
}

void LoopBus::on_fd(int, void *data) { static_cast<LoopBus *>(data)->dispatch(); }

void LoopBus::on_timeout(void *data) {
  auto *loop = static_cast<LoopBus *>(data);
  loop->timeout_armed_ = false;
  loop->dispatch();
}

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
                void *userdata) {
  if (!message) return false;
  const int started = sd_bus_call_async(loop.bus(), nullptr, message, reply,
                                        userdata, timeout_usec);
  sd_bus_message_unref(message);
  loop.schedule();
  return started >= 0;
}

} // namespace layerc::editor::a11y::atspi
