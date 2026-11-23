#include "a11y/atspi/adaptor.hpp"

#include "a11y/atspi/events.hpp"
#include "a11y/atspi/objects.hpp"
#include "a11y/changes.hpp"
#include "a11y/fltk_actions.hpp"
#include "a11y/fltk_tree.hpp"
#include "a11y/model.hpp"
#include "steady_timeout.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::editor::a11y::atspi {
namespace {

constexpr std::uint64_t kCallDeadlineUsec = 5000000;
// Fl::add_check runs on every loop iteration; snapshots are throttled to keep
// a large editor from paying for a tree walk per event.
constexpr auto kRefreshInterval = std::chrono::milliseconds{100};
constexpr const char *kRegistryName = "org.a11y.atspi.Registry";
constexpr const char *kRegistryPath = "/org/a11y/atspi/registry";
constexpr const char *kRegistryInterface = "org.a11y.atspi.Registry";
constexpr const char *kSocketInterface = "org.a11y.atspi.Socket";

constexpr const char *kListenerAdded =
    "type='signal',sender='org.a11y.atspi.Registry',"
    "path='/org/a11y/atspi/registry',interface='org.a11y.atspi.Registry',"
    "member='EventListenerRegistered'";
constexpr const char *kListenerRemoved =
    "type='signal',sender='org.a11y.atspi.Registry',"
    "path='/org/a11y/atspi/registry',interface='org.a11y.atspi.Registry',"
    "member='EventListenerDeregistered'";
constexpr const char *kRegistryOwner =
    "type='signal',sender='org.freedesktop.DBus',"
    "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
    "arg0='org.a11y.atspi.Registry'";

[[nodiscard]] std::string error_text(const int code) {
  return std::string{std::strerror(code < 0 ? -code : code)};
}

[[nodiscard]] std::string error_name(sd_bus_message *message) {
  const sd_bus_error *error = sd_bus_message_get_error(message);
  if (!error || !error->name) return "an unknown error";
  return error->name;
}

// Whether an error reply means "no registry is running", which is the normal
// state of an accessibility bus nobody listens to, as opposed to a registry
// that refused the application.
[[nodiscard]] bool registry_missing(sd_bus_message *message) {
  const sd_bus_error *error = sd_bus_message_get_error(message);
  if (!error) return false;
  return sd_bus_error_has_name(error, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_NAME_HAS_NO_OWNER);
}

} // namespace

// Clears the walk guard on every return path, including the rejected publish.
struct WalkGuard {
  explicit WalkGuard(bool &flag) : flag(flag) { flag = true; }
  ~WalkGuard() { flag = false; }
  WalkGuard(const WalkGuard &) = delete;
  WalkGuard &operator=(const WalkGuard &) = delete;
  bool &flag;
};

struct Adaptor::Impl {
  explicit Impl(FltkTree &exported) : tree(exported) {
    source.tree = &tree;
  }

  ~Impl() { shutdown(); }

  void start();
  void shutdown();
  void update_status();
  static void on_status_notify(void *data);

  void on_status(const StatusReport &report);
  bool open_a11y(const std::string &address);
  void close_a11y();
  void refresh();
  void ensure_current();
  // Marks the published generation stale; the action layer calls it after a
  // step took effect, which must not walk inside the notification.
  static void on_action_changed(void *data);
  static int on_request(sd_bus_message *message, void *data,
                        sd_bus_error *error);
  void embed();
  void ask_registered_events();
  void add_listener(std::string bus, const std::string &event);
  void remove_listener(const std::string &bus, const std::string &event);
  bool add_matches();

  [[nodiscard]] NodeId announcement_node(const Snapshot &snapshot) const;

  static void on_check(void *data);
  void on_refresh_timer();
  static void on_a11y_failed(void *data);
  static void on_bus_loss(void *data);
  static int on_embedded(sd_bus_message *message, void *data,
                         sd_bus_error *error);
  static int on_registered(sd_bus_message *message, void *data,
                           sd_bus_error *error);
  static int on_listener(sd_bus_message *message, void *data,
                         sd_bus_error *error);
  static int on_registry_owner(sd_bus_message *message, void *data,
                               sd_bus_error *error);

  FltkTree &tree;
  Model model;
  // The published generation, which the D-Bus objects read: both live on the
  // FLTK thread, so no read crosses a thread boundary and no request copies
  // the tree.
  Snapshot served;
  ObjectSource source;
  std::unique_ptr<LoopBus> session;
  std::unique_ptr<LoopBus> a11y;
  std::unique_ptr<StatusWatch> watch;

  sd_bus_slot *accessible_slot{};
  sd_bus_slot *application_slot{};
  sd_bus_slot *cache_slot{};
  sd_bus_slot *interface_slots[kInterfaceObjects]{};
  sd_bus_slot *listener_slot{};
  sd_bus_slot *listener_gone_slot{};
  sd_bus_slot *owner_slot{};
  sd_bus_slot *request_slot{};

  std::vector<Listener> listeners;
  bool listeners_known{};
  std::string address;
  std::string announcement;
  std::uint64_t announcement_sequence{};
  Fl_Widget *announcement_source{};

  StatusState watch_state{StatusState::off};
  std::string watch_reason;
  // A registry that answered Embed with a refusal, and a lost accessibility
  // connection: both outlive the status report that revealed them.
  std::string registry_fault;
  std::string bus_fault;
  // Why the last open_a11y() failed. It is stored into bus_fault only after the
  // failed connection is closed, so the reason survives close_a11y().
  std::string open_fault;
  std::string rejection;
  AdaptorStatus status;
  StatusHandler status_handler{};
  void *status_handler_data{};
  bool status_notify_armed{};
  std::uint64_t snapshots{};
  std::uint64_t walks{};
  // Serving a request walks at most once per interval, plus once per action the
  // client itself made take effect; a walk in progress is never re-entered.
  std::chrono::steady_clock::time_point last_walk{};
  bool actions_dirty{};
  bool walking{};
  std::chrono::steady_clock::time_point next_refresh{};
  bool check_registered{};
  SteadyTimeout refresh_timer{[this] { on_refresh_timer(); }};
  // Set by a walk that ran from the refresh timer; the check that follows sees it
  // and stays quiet, so an idle loop performs no periodic walk at all.
  bool walked_in_check{};
  bool bus_loss_armed{};
  bool started{};
};

// --- lifecycle ------------------------------------------------------------

void Adaptor::Impl::start() {
  if (started) return;
  started = true;
  // The standard opt-out GTK and Qt honour: no bus, no watch, nothing exported.
  if (const char *disable = std::getenv("NO_AT_BRIDGE");
      disable != nullptr && std::string_view{disable} == "1") {
    watch_state = StatusState::unavailable;
    watch_reason = "disabled by NO_AT_BRIDGE=1";
    update_status();
    return;
  }
  sd_bus *bus = nullptr;
  int started_bus = sd_bus_open_user(&bus);
  if (started_bus < 0) {
    watch_state = StatusState::unavailable;
    watch_reason = "the session bus is unavailable: " + error_text(started_bus);
    update_status();
    return;
  }
  session = std::make_unique<LoopBus>(bus);
  if (!session->attach()) {
    watch_state = StatusState::unavailable;
    watch_reason = session->failure();
    update_status();
    return;
  }
  watch = std::make_unique<StatusWatch>();
  watch->start(*session,
               [this](const StatusReport &report) { on_status(report); });
}

void Adaptor::Impl::shutdown() {
  if (status_notify_armed) {
    Fl::remove_timeout(on_status_notify, this);
    status_notify_armed = false;
  }
  status_handler = nullptr;
  status_handler_data = nullptr;
  close_a11y();
  // The watch's signal slots belong to the session connection: they are
  // released before it, or the bus would free them first.
  watch.reset();
  session.reset();
}

void Adaptor::Impl::update_status() {
  AdaptorStatus next;
  switch (watch_state) {
  case StatusState::unavailable:
    next = {StatusState::unavailable, watch_reason};
    break;
  case StatusState::off:
    next = {StatusState::off, watch_reason};
    break;
  case StatusState::on:
    if (!a11y || a11y->failed()) {
      next = {StatusState::unavailable,
              !bus_fault.empty()
                  ? bus_fault
                  : (a11y ? a11y->failure()
                          : std::string{"the accessibility bus is not "
                                        "connected"})};
    } else if (!registry_fault.empty()) {
      next = {StatusState::unavailable, registry_fault};
    } else if (!rejection.empty()) {
      next = {StatusState::unavailable, rejection};
    } else {
      next = {StatusState::on, {}};
    }
    break;
  }
  if (next.state == status.state && next.reason == status.reason) return;
  status = std::move(next);
  // Deferred to the loop: this runs inside a bus dispatch, and a handler may
  // destroy the adaptor, which must not happen with the dispatch on the stack.
  if (status_handler && !status_notify_armed) {
    status_notify_armed = true;
    Fl::add_timeout(0.0, on_status_notify, this);
  }
}

void Adaptor::Impl::on_status_notify(void *data) {
  auto *impl = static_cast<Impl *>(data);
  impl->status_notify_armed = false;
  if (impl->status_handler) impl->status_handler(impl->status_handler_data);
}

void Adaptor::Impl::on_status(const StatusReport &report) {
  watch_state = report.state;
  watch_reason = report.reason;
  if (report.state == StatusState::on) {
    if (!open_a11y(report.address)) {
      // close_a11y() clears the faults of the connection it releases, so the
      // failure reason is handed over to bus_fault afterwards: the status then
      // names the real cause instead of a generic "not connected".
      std::string why = std::move(open_fault);
      close_a11y();
      bus_fault = why.empty() ? "the accessibility bus is not connected"
                              : std::move(why);
    }
  } else {
    close_a11y();
  }
  update_status();
}

bool Adaptor::Impl::open_a11y(const std::string &bus_address) {
  if (a11y && bus_address == address) return true;
  if (a11y) close_a11y();
  open_fault.clear();
  if (bus_address.empty()) {
    open_fault = "the accessibility bus address is empty";
    return false;
  }
  sd_bus *bus = nullptr;
  int problem = sd_bus_new(&bus);
  if (problem >= 0) problem = sd_bus_set_address(bus, bus_address.c_str());
  if (problem >= 0) problem = sd_bus_set_bus_client(bus, 1);
  if (problem >= 0) problem = sd_bus_start(bus);
  if (problem < 0) {
    open_fault = "the accessibility bus is unavailable: " + error_text(problem);
    sd_bus_unref(bus);
    return false;
  }
  auto loop = std::make_unique<LoopBus>(bus);
  loop->set_failure_handler(on_a11y_failed, this);
  if (!loop->attach()) {
    open_fault = loop->failure();
    return false;
  }
  a11y = std::move(loop);
  bus_fault.clear();
  address = bus_address;
  layerc::editor::a11y::set_changed_handler(on_action_changed, this);
  const char *unique = nullptr;
  if (sd_bus_get_unique_name(a11y->bus(), &unique) < 0) unique = nullptr;
  source.unique_name = unique != nullptr ? unique : "";
  source.snapshot = nullptr;
  source.root = 0;
  served = Snapshot{};
  add_accessible_object(a11y->bus(), &source, &accessible_slot);
  add_application_object(a11y->bus(), &source, &application_slot);
  add_cache_object(a11y->bus(), &source, &cache_slot);
  add_interface_objects(a11y->bus(), &source, interface_slots);
  if (!add_matches()) {
    // close_a11y() clears the faults of the connection it releases, so the
    // reason survives the teardown it triggers here.
    std::string why = std::move(open_fault);
    close_a11y();
    open_fault = std::move(why);
    return false;
  }
  Fl::add_check(on_check, this);
  check_registered = true;
  next_refresh = std::chrono::steady_clock::now();
  refresh();
  if (a11y && !a11y->failed()) {
    embed();
    ask_registered_events();
  }
  if (a11y && a11y->failed()) {
    open_fault = a11y->failure();
    return false;
  }
  return true;
}

bool Adaptor::Impl::add_matches() {
  sd_bus *bus = a11y->bus();
  // Requests are served with the generation their own action produced: the
  // filter runs before the object handlers and never swallows a message.
  if (sd_bus_add_filter(bus, &request_slot, on_request, this) < 0) {
    open_fault = "the accessibility bus refused the request watch";
    return false;
  }
  if (sd_bus_add_match(a11y->bus(), &listener_slot, kListenerAdded, on_listener,
                       this) < 0 ||
      sd_bus_add_match(a11y->bus(), &listener_gone_slot, kListenerRemoved,
                       on_listener, this) < 0 ||
      sd_bus_add_match(a11y->bus(), &owner_slot, kRegistryOwner,
                       on_registry_owner, this) < 0) {
    open_fault = "the accessibility bus refused the registry watches";
    return false;
  }
  return true;
}

void Adaptor::Impl::close_a11y() {
  if (check_registered) {
    Fl::remove_check(on_check, this);
    check_registered = false;
  }
  refresh_timer.cancel();
  layerc::editor::a11y::set_changed_handler(nullptr, nullptr);
  if (bus_loss_armed) {
    Fl::remove_timeout(on_bus_loss, this);
    bus_loss_armed = false;
  }
  if (a11y) {
    if (!a11y->failed() && !source.unique_name.empty()) {
      sd_bus_message *message = nullptr;
      if (sd_bus_message_new_method_call(a11y->bus(), &message, kRegistryName,
                                         kRegistryPath, kRegistryInterface,
                                         "DeregisterApplication") >= 0) {
        const char *unique = source.unique_name.c_str();
        if (sd_bus_message_append_basic(message, SD_BUS_TYPE_STRING, unique) >=
            0)
          sd_bus_send(a11y->bus(), message, nullptr);
        sd_bus_message_unref(message);
      }
    }
    sd_bus_slot_unref(accessible_slot);
    sd_bus_slot_unref(application_slot);
    sd_bus_slot_unref(cache_slot);
    for (auto *&slot : interface_slots) {
      sd_bus_slot_unref(slot);
      slot = nullptr;
    }
    sd_bus_slot_unref(listener_slot);
    sd_bus_slot_unref(listener_gone_slot);
    sd_bus_slot_unref(owner_slot);
    sd_bus_slot_unref(request_slot);
    accessible_slot = nullptr;
    application_slot = nullptr;
    cache_slot = nullptr;
    listener_slot = nullptr;
    listener_gone_slot = nullptr;
    owner_slot = nullptr;
    request_slot = nullptr;
    a11y.reset();
  }
  address.clear();
  listeners.clear();
  listeners_known = false;
  announcement.clear();
  announcement_sequence = 0;
  announcement_source = nullptr;
  source = ObjectSource{};
  served = Snapshot{};
  model.clear();
  tree.clear();
  rejection.clear();
  registry_fault.clear();
  bus_fault.clear();
  open_fault.clear();
  snapshots = 0;
  walks = 0;
  actions_dirty = false;
  walking = false;
  last_walk = std::chrono::steady_clock::time_point{};
}

// --- snapshot publishing --------------------------------------------------

NodeId Adaptor::Impl::announcement_node(const Snapshot &snapshot) const {
  if (announcement_source) {
    const NodeId id = tree.id_for(*announcement_source);
    if (id != no_node) return id;
  }
  // As the legacy bridge did: a source that is not exported speaks from the
  // active window, and with no window active, from the application.
  for (const auto &[id, node] : snapshot.nodes)
    if (id != snapshot.root && node.parent == snapshot.root && node.states.active)
      return id;
  return snapshot.root;
}

void Adaptor::Impl::on_check(void *data) {
  auto *impl = static_cast<Impl *>(data);
  if (impl->walked_in_check) {
    impl->walked_in_check = false;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < impl->next_refresh) {
    // Checks run once per loop iteration, so a change made inside the window
    // waits for unrelated input unless the remaining interval is armed.
    if (!impl->refresh_timer.pending()) impl->refresh_timer.start(impl->next_refresh - now);
    return;
  }
  impl->next_refresh = now + kRefreshInterval;
  impl->refresh();
}

void Adaptor::Impl::on_refresh_timer() {
  const auto now = std::chrono::steady_clock::now();
  if (now < next_refresh) {
    refresh_timer.start(next_refresh - now);
    return;
  }
  next_refresh = now + kRefreshInterval;
  refresh();
  // A walk from this timer is the loop's only activity in this pass: the next
  // check runs before anything else can have changed the tree, so it stays
  // quiet instead of re-arming and walking again.
  walked_in_check = true;
}

// Called from inside LoopBus::dispatch: the connection that is dispatching
// must outlive the callback, so the recovery runs on the next loop iteration.
void Adaptor::Impl::on_a11y_failed(void *data) {
  auto *impl = static_cast<Impl *>(data);
  if (impl->bus_loss_armed) return;
  impl->bus_loss_armed = true;
  Fl::add_timeout(0.0, on_bus_loss, impl);
}

void Adaptor::Impl::on_bus_loss(void *data) {
  auto *impl = static_cast<Impl *>(data);
  impl->bus_loss_armed = false;
  const std::string reason = impl->a11y ? impl->a11y->failure() : std::string{};
  impl->close_a11y();
  impl->bus_fault = reason.empty() ? "the accessibility bus connection failed"
                                   : reason;
  impl->update_status();
}

void Adaptor::Impl::refresh() {
  if (!a11y || a11y->failed()) return;
  // The walk publishes, and publishing emits: a request that arrives during it
  // must not nest a second walk on the one whose events are still going out.
  if (walking) return;
  WalkGuard guard{walking};
  last_walk = std::chrono::steady_clock::now();
  // The request walk and the periodic check share one cadence: neither walks on
  // the heels of the other, so a client polling in a tight loop never sees two
  // walks in one interval.
  next_refresh = last_walk + kRefreshInterval;
  // A timer armed for the previous cadence would walk again on the heels of
  // this one; the next check re-arms it for the new deadline.
  refresh_timer.cancel();
  actions_dirty = false;
  ++walks;
  // The previous generation moves into the diff; nothing is served until the
  // next one is published, and no request can interleave on this thread.
  Snapshot before = std::move(served);
  source.snapshot = nullptr;
  Snapshot next = tree.snapshot();
  if (!announcement.empty()) {
    next.announcement = announcement;
    next.announcement_sequence = announcement_sequence;
    next.announcement_source = announcement_node(next);
  }
  if (const auto error = model.publish(next)) {
    rejection = "the accessibility tree was rejected: " + error->message;
    served = std::move(before);
    source.snapshot = &served;
    // The request that triggered this walk is served the old generation, so the
    // next one walks again instead of waiting out the interval.
    actions_dirty = true;
    update_status();
    return;
  }
  rejection.clear();
  const bool baseline = snapshots == 0;
  const std::vector<Change> differences =
      baseline ? std::vector<Change>{} : changes(before, next);
  ++snapshots;
  served = std::move(next);
  source.snapshot = &served;
  source.root = served.root;
  update_status();
  // The first snapshot is what clients fetch through Cache.GetItems; only
  // later differences are announced as events.
  if (!baseline)
    emit_events(*a11y, source, before, served, differences,
                listeners_known ? &listeners : nullptr);
}

// --- requests -------------------------------------------------------------

void Adaptor::Impl::ensure_current() {
  if (!a11y || a11y->failed() || walking) return;
  // A listener-less client that polls in a tight loop pays at most one walk per
  // interval, plus one for each action of its own that took effect.
  if (!actions_dirty &&
      std::chrono::steady_clock::now() - last_walk < kRefreshInterval)
    return;
  refresh();
}

void Adaptor::Impl::on_action_changed(void *data) {
  static_cast<Impl *>(data)->actions_dirty = true;
}

int Adaptor::Impl::on_request(sd_bus_message *message, void *data,
                              sd_bus_error *) {
  auto *impl = static_cast<Impl *>(data);
  if (sd_bus_message_is_method_call(message, nullptr, nullptr) > 0)
    impl->ensure_current();
  return 0;
}

// --- registry -------------------------------------------------------------

void Adaptor::Impl::embed() {
  if (!a11y || a11y->failed()) return;
  sd_bus_message *call = nullptr;
  if (sd_bus_message_new_method_call(a11y->bus(), &call, kRegistryName,
                                     std::string{kRootPath}.c_str(),
                                     kSocketInterface, "Embed") < 0) {
    registry_fault = "the registry call could not be built";
    update_status();
    return;
  }
  if (append_reference(call, source.unique_name, source.root, source.root) <
      0) {
    sd_bus_message_unref(call);
    registry_fault = "the registry call could not be built";
    update_status();
    return;
  }
  if (!call_async(*a11y, call, kCallDeadlineUsec, on_embedded, this))
    registry_fault = "the registry call could not be sent";
  update_status();
}

int Adaptor::Impl::on_embedded(sd_bus_message *message, void *data,
                               sd_bus_error *) {
  auto *impl = static_cast<Impl *>(data);
  if (sd_bus_message_is_method_error(message, nullptr)) {
    // Nobody listening is not a refusal: accessibility stays on until a
    // registry exists, which announce themselves with an owner change.
    if (!registry_missing(message)) {
      impl->registry_fault =
          "the registry did not accept the application: " + error_name(message);
      impl->update_status();
    }
    return 0;
  }
  impl->registry_fault.clear();
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "so") < 0) {
    impl->update_status();
    return 0;
  }
  const char *name = nullptr;
  const char *path = nullptr;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &name) >= 0 &&
      name)
    impl->source.desktop_name = name;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_OBJECT_PATH, &path) >= 0 &&
      path)
    impl->source.desktop_path = path;
  sd_bus_message_exit_container(message);
  impl->update_status();
  return 0;
}

void Adaptor::Impl::ask_registered_events() {
  if (!a11y || a11y->failed()) return;
  sd_bus_message *call = nullptr;
  if (sd_bus_message_new_method_call(a11y->bus(), &call, kRegistryName,
                                     kRegistryPath, kRegistryInterface,
                                     "GetRegisteredEvents") < 0)
    return;
  if (!call_async(*a11y, call, kCallDeadlineUsec, on_registered, this)) return;
}

int Adaptor::Impl::on_registered(sd_bus_message *message, void *data,
                                 sd_bus_error *) {
  auto *impl = static_cast<Impl *>(data);
  if (sd_bus_message_is_method_error(message, nullptr)) return 0;
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ss)") < 0)
    return 0;
  while (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ss") >
         0) {
    const char *bus = nullptr;
    const char *event = nullptr;
    if (sd_bus_message_read(message, "ss", &bus, &event) < 0) break;
    if (event && *event) impl->add_listener(bus ? bus : "", event);
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  // Only the registry's complete answer decides which events are wanted; a
  // single listener signal says nothing about the ones it did not name.
  impl->listeners_known = true;
  return 0;
}

// Registrations are kept in the registry's spelling, so its answer
// ("Object:TextChanged:") and its signal ("Object:TextChanged") for the same
// registration are one listener.
void Adaptor::Impl::add_listener(std::string bus,
                                 const std::string &event) {
  std::string canonical = canonical_event(event);
  if (canonical.empty()) return;
  const auto known = std::find_if(
      listeners.begin(), listeners.end(), [&](const Listener &listener) {
        return listener.bus == bus && listener.event == canonical;
      });
  if (known == listeners.end())
    listeners.push_back(Listener{std::move(bus), std::move(canonical)});
}

// The registry withdraws every registration of the bus that the pattern
// covers; an empty pattern is a listener that left the bus.
void Adaptor::Impl::remove_listener(const std::string &bus,
                                    const std::string &event) {
  std::erase_if(listeners, [&](const Listener &listener) {
    return listener.bus == bus && covers(event, listener.event);
  });
}

int Adaptor::Impl::on_listener(sd_bus_message *message, void *data,
                               sd_bus_error *) {
  auto *impl = static_cast<Impl *>(data);
  const char *bus = nullptr;
  const char *event = nullptr;
  const char *member = sd_bus_message_get_member(message);
  if (sd_bus_message_read(message, "ss", &bus, &event) < 0) return 0;
  const std::string owner = bus ? bus : "";
  const std::string name = event ? event : "";
  if (member && std::string{member} == "EventListenerDeregistered")
    impl->remove_listener(owner, name);
  else
    impl->add_listener(owner, name);
  return 0;
}

int Adaptor::Impl::on_registry_owner(sd_bus_message *message, void *data,
                                     sd_bus_error *) {
  auto *impl = static_cast<Impl *>(data);
  const char *name = nullptr;
  const char *previous = nullptr;
  const char *next = nullptr;
  if (sd_bus_message_read(message, "sss", &name, &previous, &next) < 0)
    return 0;
  if (!next || !*next) {
    // The registry left; a new one announces itself before it can listen.
    impl->listeners.clear();
    impl->listeners_known = false;
    return 0;
  }
  // A different registry knows its own listeners: start from its answer.
  impl->listeners.clear();
  impl->listeners_known = false;
  impl->embed();
  impl->ask_registered_events();
  return 0;
}

// --- the public surface ---------------------------------------------------

Adaptor::Adaptor(FltkTree &tree) : impl_(std::make_unique<Impl>(tree)) {
  impl_->start();
}

Adaptor::~Adaptor() = default;

AdaptorStatus Adaptor::status() const { return impl_->status; }

std::uint64_t Adaptor::walks_for_testing() const { return impl_->walks; }

void Adaptor::on_status_change(StatusHandler handler, void *data) {
  impl_->status_handler = handler;
  impl_->status_handler_data = data;
}

void Adaptor::announce(Fl_Widget *source, std::string text) {
  if (!impl_->a11y || impl_->a11y->failed()) return;
  impl_->announcement = std::move(text);
  impl_->announcement_source = source;
  ++impl_->announcement_sequence;
}

std::uint64_t Adaptor::snapshot_count() const { return impl_->snapshots; }

bool Adaptor::connected() const {
  return impl_->a11y != nullptr && !impl_->a11y->failed();
}

bool Adaptor::watching() const { return impl_->check_registered; }

} // namespace layerc::editor::a11y::atspi
