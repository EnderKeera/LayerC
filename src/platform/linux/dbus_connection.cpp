#include "dbus_connection.hpp"

#include <poll.h>

#include <chrono>
#include <limits>
#include <utility>

namespace layerc::platform {
namespace {

struct CallContext {
  BusReply reply;
  std::weak_ptr<int> guard;
  std::shared_ptr<bool> done;
};

struct SignalContext {
  BusSignal handler;
  std::weak_ptr<int> guard;
};

int on_reply(sd_bus_message *message, void *userdata, sd_bus_error *) {
  auto *context = static_cast<CallContext *>(userdata);
  *context->done = true;
  if (const auto alive = context->guard.lock()) {
    try {
      context->reply(message, sd_bus_message_get_error(message));
    } catch (...) {
      // A client failure never unwinds through sd-bus.
    }
  }
  return 0;
}

int on_signal(sd_bus_message *message, void *userdata, sd_bus_error *) {
  auto *context = static_cast<SignalContext *>(userdata);
  if (const auto alive = context->guard.lock()) {
    try {
      context->handler(message);
    } catch (...) {
    }
  }
  return 0;
}

void destroy_call(void *userdata) { delete static_cast<CallContext *>(userdata); }
void destroy_signal(void *userdata) { delete static_cast<SignalContext *>(userdata); }

constexpr int dispatch_budget = 1024;

} // namespace

BusConnection::~BusConnection() { close(); }

bool BusConnection::open(EventLoop &loop) {
  if (bus_ != nullptr) return true;
  sd_bus *bus{};
  if (sd_bus_open_user(&bus) < 0 || bus == nullptr) {
    reason_ = "bus_unavailable";
    return false;
  }
  const int fd = sd_bus_get_fd(bus);
  if (fd < 0) {
    sd_bus_close(bus);
    sd_bus_unref(bus);
    reason_ = "bus_unavailable";
    return false;
  }
  bus_ = bus;
  loop_ = &loop;
  fd_ = fd;
  events_ = 0;
  reason_.clear();
  guard_ = make_bus_guard();
  sync_registration();
  return true;
}

void BusConnection::close() noexcept {
  if (bus_ == nullptr) return;
  if (loop_ != nullptr && fd_ >= 0) loop_->unregister_fd(fd_);
  // Slots go before the bus so their destroy callbacks run against it.
  watches_.clear();
  pending_.clear();
  released_.clear();
  guard_.reset();
  // The owner drains queued teardown writes on its event loop before close.
  // sd_bus_flush would synchronously wait on a congested peer.
  sd_bus_close(bus_);
  sd_bus_unref(std::exchange(bus_, nullptr));
  fd_ = -1;
  events_ = 0;
  owners_changed_ = true;
}

bool BusConnection::outgoing_pending() const noexcept {
  std::uint64_t queued{};
  return bus_ && sd_bus_get_n_queued_write(bus_, &queued) >= 0 && queued != 0;
}

void BusConnection::lost() noexcept {
  close();
  reason_ = "bus_connection_lost";
}

std::string BusConnection::unique_name() const {
  const char *name{};
  if (bus_ == nullptr || sd_bus_get_unique_name(bus_, &name) < 0 || name == nullptr) return {};
  return name;
}

void BusConnection::sync_registration() noexcept {
  if (bus_ == nullptr || loop_ == nullptr || fd_ < 0) return;
  const int wanted = sd_bus_get_events(bus_);
  const short events = static_cast<short>(wanted < 0 ? POLLIN : (wanted | POLLIN));
  if (events == events_) return;
  events_ = events;
  loop_->register_fd(fd_, events);
}

std::optional<core::TimePoint> BusConnection::next_deadline() const noexcept {
  if (bus_ == nullptr) return std::nullopt;
  std::uint64_t usec{};
  if (sd_bus_get_timeout(bus_, &usec) < 0 || usec == std::numeric_limits<std::uint64_t>::max())
    return std::nullopt;
  // sd-bus reports CLOCK_MONOTONIC, the steady_clock epoch on Linux.
  return core::TimePoint{std::chrono::duration_cast<core::MonotonicClock::duration>(
      std::chrono::microseconds{usec})};
}

bool BusConnection::dispatch() {
  if (bus_ == nullptr) return false;
  bool handled = false;
  dispatching_ = true;
  for (int round = 0; round < dispatch_budget && bus_ != nullptr; ++round) {
    const int result = sd_bus_process(bus_, nullptr);
    if (result < 0) {
      dispatching_ = false;
      released_.clear();
      lost();
      return true;
    }
    if (result == 0) break;
    handled = true;
  }
  prune_calls();
  dispatching_ = false;
  released_.clear();
  if (bus_ != nullptr && sd_bus_is_open(bus_) <= 0) {
    lost();
    return true;
  }
  sync_registration();
  return handled;
}

bool BusConnection::call(const char *destination, const char *path, const char *interface,
                         const char *member, const BusAppend &append, BusReply reply,
                         const BusGuard &guard) {
  if (bus_ == nullptr) return false;
  sd_bus_message *message{};
  if (sd_bus_message_new_method_call(bus_, &message, destination, path, interface, member) < 0)
    return false;
  if (append && append(message) < 0) {
    sd_bus_message_unref(message);
    return false;
  }
  auto done = std::make_shared<bool>(false);
  auto *context = new CallContext{std::move(reply), guard, done};
  sd_bus_slot *slot{};
  const int result =
      sd_bus_call_async(bus_, &slot, message, on_reply, context, bus_call_timeout_usec);
  sd_bus_message_unref(message);
  if (result < 0) {
    delete context;
    return false;
  }
  sd_bus_slot_set_destroy_callback(slot, destroy_call);
  // Owned, not floating: release() and pruning cancel it, so a pending call
  // never outlives the client that wanted its reply.
  BusSlot owned{slot};
  prune_calls();
  try {
    pending_.push_back({std::move(owned), guard, std::move(done)});
  } catch (...) {
    return false; // `owned` cancels the call
  }
  sync_registration();
  return true;
}

void BusConnection::release(BusGuard &guard) noexcept {
  guard.reset();
  prune_calls();
}

void BusConnection::prune_calls() noexcept {
  for (std::size_t index = 0; index < pending_.size();) {
    auto &call = pending_[index];
    if (!*call.done && !call.guard.expired()) {
      ++index;
      continue;
    }
    if (dispatching_) {
      try {
        released_.push_back(std::move(call.slot));
      } catch (...) {
        ++index; // retried by the next prune, outside this callback
        continue;
      }
    }
    if (index + 1 != pending_.size()) call = std::move(pending_.back());
    pending_.pop_back();
  }
}

bool BusConnection::get_property(const char *destination, const char *path,
                                 const char *interface, const char *property, BusReply reply,
                                 const BusGuard &guard) {
  return call(
      destination, path, "org.freedesktop.DBus.Properties", "Get",
      [&](sd_bus_message *message) {
        return sd_bus_message_append(message, "ss", interface, property);
      },
      std::move(reply), guard);
}

BusSlot BusConnection::add_match(const std::string &rule, BusSignal handler,
                                 const BusGuard &guard) {
  if (bus_ == nullptr) return {};
  auto *context = new SignalContext{std::move(handler), guard};
  sd_bus_slot *slot{};
  if (sd_bus_add_match_async(bus_, &slot, rule.c_str(), on_signal, nullptr, context) < 0) {
    delete context;
    return {};
  }
  sd_bus_slot_set_destroy_callback(slot, destroy_signal);
  sync_registration();
  return BusSlot{slot};
}

void BusConnection::defer_release(BusSlot slot) {
  if (slot) released_.push_back(std::move(slot));
}

void BusConnection::set_owner(const std::string &name, std::optional<std::string> owner) {
  const auto found = watches_.find(name);
  if (found == watches_.end()) return;
  auto &watch = found->second;
  if (watch.known && watch.owner == owner) return;
  watch.known = true;
  watch.owner = std::move(owner);
  owners_changed_ = true;
}

void BusConnection::watch_name(const std::string &name) {
  if (bus_ == nullptr || watches_.contains(name)) return;
  auto &watch = watches_[name];
  // The match is installed before GetNameOwner is sent, so no transition
  // between the two can be missed.
  watch.match = add_match(
      "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
      "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" + name + "'",
      [this](sd_bus_message *signal) {
        const char *changed{};
        const char *previous{};
        const char *next{};
        if (sd_bus_message_read(signal, "sss", &changed, &previous, &next) < 0) return;
        set_owner(changed, next != nullptr && *next != '\0' ? std::optional<std::string>{next}
                                                            : std::nullopt);
      },
      guard_);
  (void)call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
      [&](sd_bus_message *message) { return sd_bus_message_append(message, "s", name.c_str()); },
      [this, name](sd_bus_message *reply, const sd_bus_error *error) {
        const char *owner{};
        if (error != nullptr || sd_bus_message_read(reply, "s", &owner) < 0 || owner == nullptr) {
          set_owner(name, std::nullopt);
          return;
        }
        set_owner(name, std::string{owner});
      },
      guard_);
}

std::optional<std::string> BusConnection::owner(const std::string_view name) const {
  const auto found = watches_.find(name);
  return found == watches_.end() ? std::nullopt : found->second.owner;
}

bool BusConnection::owner_known(const std::string_view name) const {
  const auto found = watches_.find(name);
  return found != watches_.end() && found->second.known;
}

bool BusConnection::take_owners_changed() noexcept { return std::exchange(owners_changed_, false); }

std::string portal_request_path(const std::string_view unique_name, const std::string_view token) {
  std::string sender{unique_name.starts_with(':') ? unique_name.substr(1) : unique_name};
  for (auto &character : sender)
    if (character == '.') character = '_';
  return "/org/freedesktop/portal/desktop/request/" + sender + "/" + std::string{token};
}

} // namespace layerc::platform
