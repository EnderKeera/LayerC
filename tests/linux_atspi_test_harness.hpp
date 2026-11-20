#pragma once

#include "a11y/atspi/adaptor.hpp"
#include "a11y/atspi/events.hpp"
#include "a11y/atspi/mapping.hpp"
#include "a11y/atspi/objects.hpp"
#include "a11y/fltk_tree.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Window.H>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace layerc::test::atspi_fixture {

using namespace layerc::editor::a11y;
namespace atspi = layerc::editor::a11y::atspi;
using layerc::test::expect;

using Clock = std::chrono::steady_clock;

constexpr const char *kBusName = "org.a11y.Bus";
constexpr const char *kBusPath = "/org/a11y/bus";
constexpr const char *kStatusInterface = "org.a11y.Status";
constexpr const char *kBusInterface = "org.a11y.Bus";
constexpr const char *kRegistryName = "org.a11y.atspi.Registry";
constexpr const char *kRegistryPath = "/org/a11y/atspi/registry";
constexpr const char *kRegistryInterface = "org.a11y.atspi.Registry";
constexpr const char *kSocketInterface = "org.a11y.atspi.Socket";
constexpr const char *kAccessibleInterface = "org.a11y.atspi.Accessible";
constexpr const char *kApplicationInterface = "org.a11y.atspi.Application";
constexpr const char *kCacheInterface = "org.a11y.atspi.Cache";
constexpr const char *kPropertiesInterface =
    "org.freedesktop.DBus.Properties";
constexpr std::uint64_t kDeadlineUsec = 3000000;

inline std::chrono::milliseconds ms(const long value) {
  return std::chrono::milliseconds{value};
}

// --- recorded traffic -----------------------------------------------------

struct Signal {
  std::string sender, path, interface, member;
  // Event bodies: (s name, i detail1, i detail2, v any_data).
  std::string name, text;
  std::optional<double> number;
  std::int32_t detail1{}, detail2{};
  char data{'u'};

  [[nodiscard]] bool is_event() const {
    return interface.rfind("org.a11y.atspi.Event.", 0) == 0;
  }
  [[nodiscard]] bool is_cache() const { return interface == kCacheInterface; }
};

struct OwnerChange {
  std::string name, previous, next;
};

struct Reference {
  std::string name, path;
};

// A listener the registry knows about: the bus that registered it and the
// event pattern it asked for.
struct Registered {
  std::string bus, event;
};

struct CallResult {
  bool done{};
  bool failed{};
  std::string error;
  sd_bus_message *reply{};

  CallResult() = default;
  CallResult(const CallResult &) = delete;
  CallResult &operator=(const CallResult &) = delete;
  CallResult(CallResult &&other) noexcept
      : done(other.done), failed(other.failed), error(other.error),
        reply(std::exchange(other.reply, nullptr)) {}
  CallResult &operator=(CallResult &&other) noexcept {
    if (this != &other) {
      if (reply) sd_bus_message_unref(reply);
      done = other.done;
      failed = other.failed;
      error = other.error;
      reply = std::exchange(other.reply, nullptr);
    }
    return *this;
  }
  ~CallResult() {
    if (reply) sd_bus_message_unref(reply);
  }
};

// --- the fake accessibility stack ----------------------------------------

// Owns org.a11y.Bus (status flags and GetAddress) and
// org.a11y.atspi.Registry (Embed, GetRegisteredEvents, listener signals) on the
// session bus, answers every request on the FLTK thread, and records what the
// editor's connection broadcasts. It never blocks: every call and every wait
// interleaves sd-bus processing with Fl::wait.
class Harness {
public:
  Harness() {
    if (sd_bus_open_user(&bus_) < 0) {
      bus_ = nullptr;
      return;
    }
    const char *address = nullptr;
    if (sd_bus_get_address(bus_, &address) >= 0 && address != nullptr)
      session_address_ = address;
    if (!register_objects()) {
      sd_bus_flush_close_unref(bus_);
      bus_ = nullptr;
      return;
    }
    ok_ = true;
  }

  ~Harness() {
    sd_bus_slot_unref(status_slot_);
    sd_bus_slot_unref(address_slot_);
    sd_bus_slot_unref(socket_slot_);
    sd_bus_slot_unref(registry_slot_);
    sd_bus_slot_unref(match_slot_);
    sd_bus_message_unref(held_registered_);
    for (sd_bus_message *held : held_address_) sd_bus_message_unref(held);
    if (bus_) sd_bus_flush_close_unref(bus_);
  }

  Harness(const Harness &) = delete;
  Harness &operator=(const Harness &) = delete;

  [[nodiscard]] bool is_open() const { return ok_; }
  [[nodiscard]] sd_bus *bus() const { return bus_; }
  [[nodiscard]] const std::string &session_address() const {
    return session_address_;
  }
  [[nodiscard]] const std::string &reported_address() const {
    return reported_address_.empty() ? session_address_ : reported_address_;
  }

  // --- what the harness publishes ---

  void set_flags(const bool enabled, const bool screen_reader) {
    enabled_ = enabled;
    screen_reader_ = screen_reader;
    if (!ok_) return;
    sd_bus_emit_properties_changed(bus_, kBusPath, kStatusInterface,
                                   "IsEnabled", "ScreenReaderEnabled", nullptr);
    sd_bus_flush(bus_);
  }

  void set_reported_address(std::string address) {
    reported_address_ = std::move(address);
  }

  // org.a11y.Bus answers GetAddress with a failure rather than an address.
  void set_address_error(const bool failing) { address_error_ = failing; }
  [[nodiscard]] bool address_error() const { return address_error_; }

  // The registry's answer to GetRegisteredEvents.
  void set_registered_events(std::vector<std::string> events) {
    registered_.clear();
    for (auto &event : events) registered_.push_back({"", std::move(event)});
  }

  void set_registered_listeners(std::vector<Registered> listeners) {
    registered_ = std::move(listeners);
  }

  [[nodiscard]] int append_registered_events(sd_bus_message *reply) const {
    int appended =
        sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ss)");
    for (const auto &listener : registered_) {
      if (appended < 0) break;
      appended = sd_bus_message_append(reply, "(ss)", listener.bus.c_str(),
                                       listener.event.c_str());
    }
    if (appended >= 0) appended = sd_bus_message_close_container(reply);
    return appended;
  }

  // The registry refuses Embed with this error until it is cleared.
  void set_embed_error(std::string name) { embed_error_ = std::move(name); }
  [[nodiscard]] const std::string &embed_error() const { return embed_error_; }

  // Holds GetRegisteredEvents so a listener signal can arrive before the
  // answer, as a busy registry delivers them.
  void hold_registered_replies(const bool hold) { hold_registered_ = hold; }

  // Returns true when the answer must wait for release_registered_reply().
  bool hold_registered(sd_bus_message *message) {
    if (!hold_registered_) return false;
    sd_bus_message_unref(held_registered_);
    held_registered_ = sd_bus_message_ref(message);
    return true;
  }

  bool release_registered_reply() {
    sd_bus_message *held = std::exchange(held_registered_, nullptr);
    if (held == nullptr) return false;
    sd_bus_message *reply = nullptr;
    int appended = sd_bus_message_new_method_return(held, &reply);
    if (appended >= 0) appended = append_registered_events(reply);
    if (appended >= 0 &&
        sd_bus_send(sd_bus_message_get_bus(held), reply, nullptr) < 0)
      appended = -EIO;
    sd_bus_message_unref(reply);
    sd_bus_message_unref(held);
    sd_bus_flush(bus_);
    return appended >= 0;
  }

  // Holds GetAddress answers, so a request can be answered after the status
  // that caused it is already out of date.
  void hold_address_replies(const bool hold) { hold_address_ = hold; }

  // How many GetAddress requests wait for release_address_replies().
  [[nodiscard]] std::size_t held_address_requests() const {
    return held_address_.size();
  }

  // Returns true when the answer must wait for release_address_replies().
  bool hold_address(sd_bus_message *message) {
    if (!hold_address_) return false;
    held_address_.push_back(sd_bus_message_ref(message));
    return true;
  }

  // Answers every held request with what the harness says now: the fault and
  // the address are read at release time, not when the request arrived.
  bool release_address_replies() {
    auto held = std::exchange(held_address_, {});
    if (held.empty()) return false;
    bool answered = true;
    for (sd_bus_message *request : held) {
      sd_bus_message *reply = nullptr;
      int built = 0;
      if (address_error_)
        built = sd_bus_message_new_method_errorf(
            request, &reply, SD_BUS_ERROR_FAILED,
            "the accessibility bus is unavailable");
      else
        built = sd_bus_message_new_method_return(request, &reply);
      if (built >= 0 && !address_error_)
        built = sd_bus_message_append(reply, "s", reported_address().c_str());
      if (built >= 0 &&
          sd_bus_send(sd_bus_message_get_bus(request), reply, nullptr) < 0)
        built = -EIO;
      if (built < 0) answered = false;
      sd_bus_message_unref(reply);
      sd_bus_message_unref(request);
    }
    sd_bus_flush(bus_);
    return answered;
  }

  // Withdraws or restores org.a11y.Bus to model a session without it.
  void release_bus_name() {
    if (!bus_) return;
    sd_bus_release_name(bus_, kBusName);
    sd_bus_flush(bus_);
  }

  void request_bus_name() {
    if (!bus_) return;
    sd_bus_request_name(bus_, kBusName, 0);
    sd_bus_flush(bus_);
  }

  // Withdraws or restores org.a11y.atspi.Registry to model a registry that
  // restarts and changes its mind about the application.
  void release_registry_name() {
    if (!bus_) return;
    sd_bus_release_name(bus_, kRegistryName);
    sd_bus_flush(bus_);
  }

  void request_registry_name() {
    if (!bus_) return;
    sd_bus_request_name(bus_, kRegistryName, 0);
    sd_bus_flush(bus_);
  }

  // A listener appearing after the initial answer, on a chosen bus.
  void announce_listener(const std::string &event) {
    announce_listener_from("", event);
  }

  void announce_listener_from(const std::string &bus,
                              const std::string &event) {
    registered_.push_back({bus, event});
    emit_listener("EventListenerRegistered", bus, event);
  }

  // The registry withdraws every registration of the bus that the pattern
  // covers; an empty pattern is a listener that left the bus. Only the
  // answers to later GetRegisteredEvents calls depend on this, so it mirrors
  // the registry rather than the adaptor under test.
  void deregister_listener(const std::string &bus, const std::string &event) {
    std::erase_if(registered_, [&](const Registered &listener) {
      return listener.bus == bus &&
             (event.empty() || listener.event == event ||
              atspi::covers(event, listener.event));
    });
    emit_listener("EventListenerDeregistered", bus, event);
  }

  // Drains this connection only: sd_bus_process runs no FLTK check or timer,
  // so a caller that blocks in Fl::wait keeps the editor's loop starved.
  void drain() {
    while (bus_ && sd_bus_process(bus_, nullptr) > 0) {
    }
    if (bus_) sd_bus_flush(bus_);
  }

  // --- what the editor did ---

  [[nodiscard]] const std::vector<Signal> &signals() const { return signals_; }
  [[nodiscard]] const std::vector<OwnerChange> &owners() const { return owners_; }
  [[nodiscard]] int embeds() const { return embeds_; }
  [[nodiscard]] int deregistrations() const { return deregistrations_; }
  [[nodiscard]] int registered_calls() const { return registered_calls_; }
  [[nodiscard]] const std::string &embed_name() const { return embed_name_; }
  [[nodiscard]] const std::string &embed_path() const { return embed_path_; }

  [[nodiscard]] bool owner_left(const std::string &name) const {
    for (const auto &change : owners_)
      if (change.name == name && change.next.empty()) return true;
    return false;
  }

  [[nodiscard]] int status_reads() const { return status_reads_; }
  [[nodiscard]] int address_reads() const { return address_reads_; }
  void note_status_read() const { ++status_reads_; }
  void note_address_read() const { ++address_reads_; }

  // --- the loop ---

  // A second connection the case hosts itself (a private registry) must be
  // processed here too, or its calls sit unread until the case gives up.
  void pump(std::function<void()> extra) { pump_ = std::move(extra); }

  // Processes the harness side of the bus, then lets FLTK run the editor side.
  void spin(const std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    do {
      while (bus_ && sd_bus_process(bus_, nullptr) > 0) {
      }
      if (bus_) sd_bus_flush(bus_);
      if (pump_) pump_();
      Fl::wait(0.005);
    } while (Clock::now() < deadline);
  }

  [[nodiscard]] bool spin_until(const std::function<bool()> &stop,
                                const std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
      while (bus_ && sd_bus_process(bus_, nullptr) > 0) {
      }
      if (bus_) sd_bus_flush(bus_);
      if (pump_) pump_();
      if (stop()) return true;
      Fl::wait(0.005);
    }
    return stop();
  }

  // --- calls as an assistive technology client ---

  CallResult ask(const std::string &destination, const std::string &path,
                 const std::string &interface, const std::string &member,
                 const std::function<int(sd_bus_message *)> &build = {}) {
    CallResult result;
    if (!ok_) {
      result.done = true;
      result.failed = true;
      result.error = "local/no_connection";
      return result;
    }
    sd_bus_message *call = nullptr;
    if (sd_bus_message_new_method_call(bus_, &call, destination.c_str(),
                                       path.c_str(), interface.c_str(),
                                       member.c_str()) < 0) {
      result.done = true;
      result.failed = true;
      result.error = "local/new_method_call";
      return result;
    }
    if (build && build(call) < 0) {
      sd_bus_message_unref(call);
      result.done = true;
      result.failed = true;
      result.error = "local/append";
      return result;
    }
    const int started =
        sd_bus_call_async(bus_, nullptr, call, on_reply, &result, kDeadlineUsec);
    sd_bus_message_unref(call);
    if (started < 0) {
      result.done = true;
      result.failed = true;
      result.error = "local/call_async";
      return result;
    }
    if (!spin_until([&result] { return result.done; }, ms(5000))) {
      result.done = true;
      result.failed = true;
      result.error = "local/timeout";
    }
    return result;
  }

  CallResult get(const std::string &path, const std::string &interface,
                 const std::string &member) {
    return ask(embed_name_, path, interface, member);
  }

  CallResult property(const std::string &path, const std::string &interface,
                      const std::string &name) {
    return ask(embed_name_, path, kPropertiesInterface, "Get",
               [&](sd_bus_message *call) {
                 return sd_bus_message_append(call, "ss", interface.c_str(),
                                              name.c_str());
               });
  }

  // --- handlers (public: the vtables below call them) ---

  static int on_reply(sd_bus_message *message, void *userdata,
                      sd_bus_error *error) {
    auto *result = static_cast<CallResult *>(userdata);
    if (sd_bus_message_is_method_error(message, nullptr)) {
      result->failed = true;
      const sd_bus_error *fault = sd_bus_message_get_error(message);
      if (fault && fault->name)
        result->error = fault->name;
      else if (error && error->name)
        result->error = error->name;
      else
        result->error = "unknown";
    } else {
      result->reply = sd_bus_message_ref(message);
    }
    result->done = true;
    return 0;
  }

  static int on_signal(sd_bus_message *message, void *userdata,
                       sd_bus_error *) {
    static_cast<Harness *>(userdata)->record(message);
    return 1;
  }

  [[nodiscard]] bool enabled() const { return enabled_; }
  [[nodiscard]] bool screen_reader() const { return screen_reader_; }
  void note_embed(const std::string &name, const std::string &path) {
    ++embeds_;
    embed_name_ = name;
    embed_path_ = path;
  }
  void note_deregister(const std::string &name) {
    ++deregistrations_;
    deregistered_ = name;
  }
  void note_registered_call() { ++registered_calls_; }
  [[nodiscard]] const std::string &deregistered() const { return deregistered_; }

private:
  void emit_listener(const char *member, const std::string &bus,
                     const std::string &event) {
    if (!ok_) return;
    sd_bus_message *message = nullptr;
    if (sd_bus_message_new_signal(bus_, &message, kRegistryPath,
                                  kRegistryInterface, member) < 0)
      return;
    if (sd_bus_message_append(message, "ss", bus.c_str(), event.c_str()) >= 0)
      sd_bus_send(bus_, message, nullptr);
    sd_bus_message_unref(message);
    sd_bus_flush(bus_);
  }

  bool register_objects() {
    // Owning the names is the whole fixture: a session bus that already hosts
    // an accessibility registry answers in its place, so only the primary
    // owner result (1) is acceptable.
    if (sd_bus_request_name(bus_, kBusName, 0) != 1) return false;
    if (sd_bus_add_object_vtable(bus_, &status_slot_, kBusPath,
                                 kStatusInterface, kStatusVtable, this) < 0)
      return false;
    if (sd_bus_add_object_vtable(bus_, &address_slot_, kBusPath, kBusInterface,
                                 kBusVtable, this) < 0)
      return false;
    if (sd_bus_request_name(bus_, kRegistryName, 0) != 1) return false;
    const std::string root{atspi::kRootPath};
    if (sd_bus_add_object_vtable(bus_, &socket_slot_, root.c_str(),
                                 kSocketInterface, kSocketVtable, this) < 0)
      return false;
    if (sd_bus_add_object_vtable(bus_, &registry_slot_, kRegistryPath,
                                 kRegistryInterface, kRegistryVtable, this) < 0)
      return false;
    if (sd_bus_add_match(bus_, &match_slot_, "type='signal'", on_signal,
                         this) < 0)
      return false;
    sd_bus_set_method_call_timeout(bus_, kDeadlineUsec);
    return true;
  }

  void record(sd_bus_message *message) {
    const char *sender = sd_bus_message_get_sender(message);
    const char *path = sd_bus_message_get_path(message);
    const char *interface = sd_bus_message_get_interface(message);
    const char *member = sd_bus_message_get_member(message);
    const std::string iface = interface ? interface : "";
    const std::string memb = member ? member : "";
    if (iface == "org.freedesktop.DBus" && memb == "NameOwnerChanged") {
      const char *name = nullptr;
      const char *previous = nullptr;
      const char *next = nullptr;
      if (sd_bus_message_read(message, "sss", &name, &previous, &next) >= 0)
        owners_.push_back({name ? name : "", previous ? previous : "",
                           next ? next : ""});
      return;
    }
    Signal signal;
    signal.sender = sender ? sender : "";
    signal.path = path ? path : "";
    signal.interface = iface;
    signal.member = memb;
    if (signal.is_event()) {
      const char *name = nullptr;
      if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &name) >= 0 &&
          name != nullptr)
        signal.name = name;
      std::int32_t first = 0;
      std::int32_t second = 0;
      sd_bus_message_read_basic(message, SD_BUS_TYPE_INT32, &first);
      sd_bus_message_read_basic(message, SD_BUS_TYPE_INT32, &second);
      signal.detail1 = first;
      signal.detail2 = second;
      read_data(message, signal);
    }
    signals_.push_back(std::move(signal));
  }

  static void read_data(sd_bus_message *message, Signal &signal) {
    char type = 0;
    const char *contents = nullptr;
    if (sd_bus_message_peek_type(message, &type, &contents) <= 0 ||
        type != SD_BUS_TYPE_VARIANT)
      return;
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, nullptr) <=
        0)
      return;
    if (sd_bus_message_peek_type(message, &type, nullptr) > 0) {
      signal.data = type;
      if (type == SD_BUS_TYPE_STRING) {
        const char *text = nullptr;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &text) >= 0 &&
            text != nullptr)
          signal.text = text;
      } else if (type == SD_BUS_TYPE_DOUBLE) {
        double value = 0.0;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_DOUBLE, &value) > 0)
          signal.number = value;
      } else if (type == SD_BUS_TYPE_INT32) {
        std::int32_t value = 0;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_INT32, &value) >= 0)
          signal.detail2 = signal.detail2;
      } else if (type == SD_BUS_TYPE_STRUCT) {
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") >
            0) {
          const char *name = nullptr;
          const char *path = nullptr;
          if (sd_bus_message_read(message, "so", &name, &path) >= 0) {
            signal.text = name ? name : "";
            signal.name = signal.name.empty() ? (path ? path : "") : signal.name;
          }
          sd_bus_message_exit_container(message);
        }
      }
    }
    sd_bus_message_exit_container(message);
  }

  static const sd_bus_vtable kStatusVtable[];
  static const sd_bus_vtable kBusVtable[];
  static const sd_bus_vtable kSocketVtable[];
  static const sd_bus_vtable kRegistryVtable[];

  sd_bus *bus_{};
  bool ok_{};
  bool enabled_{};
  bool screen_reader_{};
  std::string session_address_;
  std::string reported_address_;
  bool address_error_{};
  std::vector<Registered> registered_;
  std::string embed_error_;
  bool hold_registered_{};
  sd_bus_message *held_registered_{};
  bool hold_address_{};
  std::vector<sd_bus_message *> held_address_;
  std::vector<Signal> signals_;
  std::vector<OwnerChange> owners_;
  std::string embed_name_, embed_path_, deregistered_;
  int embeds_{};
  int deregistrations_{};
  int registered_calls_{};
  sd_bus_slot *status_slot_{};
  sd_bus_slot *address_slot_{};
  sd_bus_slot *socket_slot_{};
  sd_bus_slot *registry_slot_{};
  sd_bus_slot *match_slot_{};
  std::function<void()> pump_;
  mutable int status_reads_{};
  mutable int address_reads_{};
};

// --- a private accessibility bus, for the cases that need a second one ----

class Daemon {
public:
  Daemon() {
    int fds[2];
    if (::pipe(fds) != 0) return;
    const pid_t child = ::fork();
    if (child < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      return;
    }
    if (child == 0) {
      ::dup2(fds[1], STDOUT_FILENO);
      ::close(fds[0]);
      ::close(fds[1]);
      ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
               "--print-address=1", static_cast<char *>(nullptr));
      ::_exit(127);
    }
    ::close(fds[1]);
    pid_ = child;
    std::string printed;
    char buffer[512];
    while (printed.find('\n') == std::string::npos) {
      const ssize_t read = ::read(fds[0], buffer, sizeof buffer);
      if (read < 0 && errno == EINTR) continue;
      if (read <= 0) break;
      printed.append(buffer, static_cast<std::size_t>(read));
    }
    ::close(fds[0]);
    address_ = printed.substr(0, printed.find('\n'));
    if (address_.empty()) stop();
  }

  ~Daemon() { stop(); }
  Daemon(const Daemon &) = delete;
  Daemon &operator=(const Daemon &) = delete;

  [[nodiscard]] bool is_running() const {
    return pid_ > 0 && !address_.empty();
  }
  [[nodiscard]] const std::string &address() const { return address_; }

  void stop() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGKILL);
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
  }

private:
  pid_t pid_{-1};
  std::string address_;
};

struct HostState {
  int embeds{};
};

// Owns org.a11y.atspi.Registry and the socket root on a bus the test controls.
// The socket root handler and its vtable are used by the class body below, so
// they keep external linkage.
int host_socket_embed(sd_bus_message *message, void *data, sd_bus_error *error);
extern const sd_bus_vtable kHostSocketVtable[];

class RegistryHost {
public:
  explicit RegistryHost(const std::string &address) { connect(address); }
  ~RegistryHost() { close(); }
  RegistryHost(const RegistryHost &) = delete;
  RegistryHost &operator=(const RegistryHost &) = delete;

  [[nodiscard]] bool is_open() const { return bus_ != nullptr; }
  [[nodiscard]] int embeds() const { return state_.embeds; }

  // Answers the calls queued on this connection.
  void drain() {
    while (bus_ && sd_bus_process(bus_, nullptr) > 0) {
    }
    if (bus_) sd_bus_flush(bus_);
  }

  void close() {
    sd_bus_slot_unref(socket_slot_);
    socket_slot_ = nullptr;
    if (bus_) sd_bus_flush_close_unref(bus_);
    bus_ = nullptr;
  }

private:
  void connect(const std::string &address) {
    close();
    state_ = {};
    if (sd_bus_new(&bus_) < 0 ||
        // A client connection may only own names once it is marked as a bus
        // client; sd_bus_open_user() sets this on the connections it makes.
        sd_bus_set_bus_client(bus_, 1) < 0 ||
        sd_bus_set_address(bus_, address.c_str()) < 0 ||
        sd_bus_start(bus_) < 0) {
      close();
      return;
    }
    if (sd_bus_request_name(bus_, kRegistryName, 0) != 1) {
      close();
      return;
    }
    const std::string root{atspi::kRootPath};
    if (sd_bus_add_object_vtable(bus_, &socket_slot_, root.c_str(),
                                 kSocketInterface, kHostSocketVtable,
                                 &state_) < 0) {
      close();
      return;
    }
    sd_bus_flush(bus_);
  }

  sd_bus *bus_{};
  sd_bus_slot *socket_slot_{};
  HostState state_{};
};

// --- reply parsers ---------------------------------------------------------

[[nodiscard]] std::optional<std::string> variant_text(sd_bus_message *message);
[[nodiscard]] std::optional<std::string> reply_text(sd_bus_message *message);
[[nodiscard]] std::optional<std::uint32_t> reply_u32(sd_bus_message *message);
[[nodiscard]] std::optional<std::int32_t> reply_i32(sd_bus_message *message);
[[nodiscard]] std::vector<std::uint32_t> reply_u32_array(sd_bus_message *message);
[[nodiscard]] std::vector<std::string> reply_strings(sd_bus_message *message);
[[nodiscard]] std::vector<Reference> reply_references(sd_bus_message *message);
[[nodiscard]] std::optional<Reference> reply_reference(sd_bus_message *message);
[[nodiscard]] std::optional<Reference>
reply_variant_reference(sd_bus_message *message);
[[nodiscard]] std::size_t reply_relation_kinds(sd_bus_message *message);
[[nodiscard]] std::size_t reply_cache_items(sd_bus_message *message);
[[nodiscard]] bool contains(const std::vector<std::string> &values,
                            const std::string_view wanted);
[[nodiscard]] bool has_bit(const std::vector<std::uint32_t> &words,
                           const atspi::State state);

// --- bus requirements ------------------------------------------------------

// Both skip (exit code 77) instead of failing when the runner has no bus: the
// CTest wrapper always provides a private one.
void require_session_bus();
void require_private_bus(const Harness &harness);
// Every node a client can reach from the root, by path.
std::size_t reachable_objects(Harness &harness);

} // namespace layerc::test::atspi_fixture
