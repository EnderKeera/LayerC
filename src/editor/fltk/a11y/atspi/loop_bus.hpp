#pragma once

#include <cstdint>
#include <string>

#include <systemd/sd-bus.h>

namespace layerc::editor::a11y::atspi {

// sd-bus' reply callback type. sd_bus_error is an untagged struct typedef, so
// the type only exists in the systemd header and every user includes it.
using ReplyHandler = int (*)(sd_bus_message *message, void *userdata,
                             sd_bus_error *error);

// One sd-bus connection driven by the FLTK event loop: the socket joins
// Fl::add_fd and sd-bus' own dispatch timeout joins Fl::add_timeout, so every
// message and every timer fires on the FLTK thread and nothing blocks it.
class LoopBus {
public:
  // Takes ownership of the connection.
  explicit LoopBus(sd_bus *bus);
  ~LoopBus();
  LoopBus(const LoopBus &) = delete;
  LoopBus &operator=(const LoopBus &) = delete;

  [[nodiscard]] sd_bus *bus() const { return bus_; }
  [[nodiscard]] bool failed() const { return !failure_.empty(); }
  [[nodiscard]] const std::string &failure() const { return failure_; }

  // Watches the socket and drains what sd-bus already queued. Returns false
  // with failure() set when the connection is unusable.
  bool attach();

  // Recomputes the watched events and the dispatch timeout after a send. Safe
  // inside a dispatch callback.
  void schedule();

  // Reports an unusable connection once, from inside dispatch(). The owner must
  // react deferred (a timeout) and never destroy the bus that is dispatching.
  using FailureHandler = void (*)(void *userdata);
  void set_failure_handler(FailureHandler handler, void *userdata) {
    failure_handler_ = handler;
    failure_handler_data_ = userdata;
  }

private:
  // One callback drains at most this many messages: a peer that floods the
  // socket must not starve the rest of the FLTK loop.
  static constexpr int kMaxMessagesPerDispatch = 64;

  static void on_fd(int fd, void *data);
  static void on_timeout(void *data);
  // Continues a bounded drain on the next loop iteration, so the loop yields
  // between batches. It has its own callback so schedule() cannot cancel it.
  static void on_continue(void *data);
  void dispatch();
  void fail(std::string why);
  // Releases the socket watch. The registered number is remembered: after a
  // failure sd_bus_get_fd() only reports the error, and a number it never
  // handed to Fl::add_fd must not be removed from the watch list.
  void release_fd();
  [[nodiscard]] bool watch_fd();

  sd_bus *bus_;
  // -1 is "no socket watched": descriptor 0 is a valid socket when stdin was
  // closed before the connection was opened.
  int fd_{-1};
  bool read_registered_{false};
  bool write_registered_{false};
  bool timeout_armed_{false};
  bool continue_armed_{false};
  std::string failure_;
  FailureHandler failure_handler_{};
  void *failure_handler_data_{};
  bool reported_{false};
};

// Queues an outgoing message (takes ownership); arms the loop afterwards.
[[nodiscard]] bool send_message(LoopBus &loop, sd_bus_message *message);

// Starts an asynchronous call (never a blocking round trip) and arms the loop.
// The reply handler runs on the FLTK thread. With a slot pointer the caller
// owns the slot and must unref it once the reply has been handled: unreffing a
// pending slot cancels the call, so its handler never runs. Without one the
// call is floating and sd-bus releases the slot when it finishes.
[[nodiscard]] bool call_async(LoopBus &loop, sd_bus_message *message,
                              std::uint64_t timeout_usec, ReplyHandler reply,
                              void *userdata, sd_bus_slot **slot = nullptr);

} // namespace layerc::editor::a11y::atspi
