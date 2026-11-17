#include "a11y/atspi/status.hpp"

#include <systemd/sd-bus.h>

#include <cstdlib>

namespace layerc::editor::a11y::atspi {
namespace {

constexpr std::uint64_t kQueryDeadlineUsec = 5000000;
constexpr const char *kSessionProperties =
    "type='signal',sender='org.a11y.Bus',path='/org/a11y/bus',"
    "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
    "arg0='org.a11y.Status'";
constexpr const char *kSessionOwner =
    "type='signal',sender='org.freedesktop.DBus',"
    "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
    "arg0='org.a11y.Bus'";

[[nodiscard]] const char *env_address() {
  const char *address = std::getenv("AT_SPI_BUS_ADDRESS");
  return address && *address ? address : nullptr;
}

// Whether an error means "org.a11y.Bus is not there", as opposed to "it is
// there and broke".
[[nodiscard]] bool missing_service(sd_bus_message *message) {
  const sd_bus_error *error = sd_bus_message_get_error(message);
  if (!error) return false;
  return sd_bus_error_has_name(error, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_NAME_HAS_NO_OWNER) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_OBJECT) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_INTERFACE) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_METHOD) ||
         sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_PROPERTY);
}

[[nodiscard]] std::string error_name(sd_bus_message *message) {
  const sd_bus_error *error = sd_bus_message_get_error(message);
  if (!error || !error->name) return "an unknown error";
  return error->name;
}

// Reads IsEnabled and ScreenReaderEnabled out of a property dictionary; a
// missing entry leaves its flag alone.
void read_status(sd_bus_message *message, bool &enabled, bool &screen_reader) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
    return;
  while (sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") >
         0) {
    const char *key = nullptr;
    if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &key) < 0)
      break;
    int value = 0;
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "b") > 0) {
      sd_bus_message_read_basic(message, SD_BUS_TYPE_BOOLEAN, &value);
      sd_bus_message_exit_container(message);
    }
    if (key) {
      if (std::string{key} == "IsEnabled") enabled = value != 0;
      if (std::string{key} == "ScreenReaderEnabled") screen_reader = value != 0;
    }
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
}

} // namespace

StatusWatch::StatusWatch() = default;

StatusWatch::~StatusWatch() {
  sd_bus_slot_unref(owner_match_);
  sd_bus_slot_unref(properties_match_);
  sd_bus_slot_unref(address_slot_);
  sd_bus_slot_unref(call_slot_);
}

void StatusWatch::start(LoopBus &session, StatusHandler report) {
  session_ = &session;
  report_ = std::move(report);
  if (!session_ || session_->failed()) {
    unavailable("the session bus is unavailable");
    return;
  }
  if (sd_bus_add_match(session_->bus(), &owner_match_, kSessionOwner, on_owner,
                       this) < 0) {
    unavailable("the session bus refused the org.a11y.Bus watch");
    return;
  }
  if (sd_bus_add_match(session_->bus(), &properties_match_,
                       kSessionProperties, on_properties, this) < 0) {
    unavailable("the session bus refused the org.a11y.Status watch");
    return;
  }
  probe();
}

void StatusWatch::probe() {
  if (!session_ || session_->failed() || probing_) return;
  sd_bus_message *call = nullptr;
  if (sd_bus_message_new_method_call(session_->bus(), &call, "org.a11y.Bus",
                                     "/org/a11y/bus",
                                     "org.freedesktop.DBus.Properties",
                                     "GetAll") < 0 ||
      sd_bus_message_append(call, "s", "org.a11y.Status") < 0) {
    sd_bus_message_unref(call);
    unavailable("cannot query org.a11y.Status");
    return;
  }
  probing_ = true;
  if (!call_async(*session_, call, kQueryDeadlineUsec, on_answers, this))
    unavailable("cannot query org.a11y.Status");
}

void StatusWatch::ask_address() {
  if (!session_ || session_->failed()) {
    unavailable("the session bus is unavailable");
    return;
  }
  sd_bus_message *call = nullptr;
  if (sd_bus_message_new_method_call(session_->bus(), &call, "org.a11y.Bus",
                                     "/org/a11y/bus", "org.a11y.Bus",
                                     "GetAddress") < 0) {
    unavailable("cannot ask org.a11y.Bus for an address");
    return;
  }
  if (!call_async(*session_, call, kQueryDeadlineUsec, on_address, this))
    unavailable("cannot ask org.a11y.Bus for an address");
}

void StatusWatch::apply() {
  // A fixed address means the environment supplies the accessibility bus, as
  // inside a Flatpak sandbox: the flags live on a bus this process cannot see,
  // so the address alone decides that accessibility is wanted.
  if (const char *address = env_address()) {
    publish(StatusReport{StatusState::on, {}, address});
    return;
  }
  if (!enabled_ && !screen_reader_) {
    publish(StatusReport{StatusState::off, {}, {}});
    return;
  }
  ask_address();
}

void StatusWatch::unavailable(std::string reason, std::string address) {
  probing_ = false;
  publish(StatusReport{StatusState::unavailable, std::move(reason),
                       std::move(address)});
}

void StatusWatch::read_status_reply(sd_bus_message *message) {
  read_status(message, enabled_, screen_reader_);
  answered_ = true;
  probing_ = false;
  apply();
}

void StatusWatch::publish(StatusReport report) {
  if (report.state == last_.state && report.reason == last_.reason &&
      report.address == last_.address)
    return;
  last_ = std::move(report);
  if (report_) report_(last_);
}

int StatusWatch::on_properties(sd_bus_message *message, void *data,
                               sd_bus_error *) {
  auto *watch = static_cast<StatusWatch *>(data);
  const char *interface = nullptr;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &interface) < 0)
    return 0;
  if (!interface || std::string{interface} != "org.a11y.Status") return 0;
  read_status(message, watch->enabled_, watch->screen_reader_);
  watch->answered_ = true;
  watch->probing_ = false;
  watch->apply();
  return 0;
}

int StatusWatch::on_owner(sd_bus_message *message, void *data, sd_bus_error *) {
  auto *watch = static_cast<StatusWatch *>(data);
  const char *name = nullptr;
  const char *previous = nullptr;
  const char *next = nullptr;
  if (sd_bus_message_read(message, "sss", &name, &previous, &next) < 0)
    return 0;
  if (!next || !*next) {
    // The bus went away; a Flatpak address survives it.
    watch->enabled_ = false;
    watch->screen_reader_ = false;
    watch->answered_ = false;
    if (const char *address = env_address())
      watch->publish(StatusReport{StatusState::on, {}, address});
    else
      watch->publish(StatusReport{StatusState::off,
                                  "org.a11y.Bus is not on the session bus", {}});
    return 0;
  }
  // The bus appeared after us; ask again. A new owner is a new accessibility
  // configuration, so the next report must be delivered even when the flags
  // read the same: a consumer that lost the old bus reopens on it.
  watch->last_ = StatusReport{};
  watch->enabled_ = false;
  watch->screen_reader_ = false;
  watch->answering_ = false;
  watch->probe();
  return 0;
}

int StatusWatch::on_answers(sd_bus_message *message, void *data,
                            sd_bus_error *) {
  auto *watch = static_cast<StatusWatch *>(data);
  watch->probing_ = false;
  if (sd_bus_message_is_method_error(message, nullptr)) {
    if (!missing_service(message)) {
      watch->unavailable("org.a11y.Status cannot be read: " +
                         error_name(message));
      return 0;
    }
    if (const char *address = env_address()) {
      watch->publish(StatusReport{StatusState::on, {}, address});
      return 0;
    }
    if (!watch->answering_) {
      watch->answering_ = true;
      watch->ask_address();
      return 0;
    }
    watch->unavailable("org.a11y.Bus is not on the session bus");
    return 0;
  }
  watch->read_status_reply(message);
  return 0;
}

int StatusWatch::on_address(sd_bus_message *message, void *data,
                            sd_bus_error *) {
  auto *watch = static_cast<StatusWatch *>(data);
  watch->answering_ = false;
  if (sd_bus_message_is_method_error(message, nullptr)) {
    watch->unavailable("org.a11y.Bus has no accessible bus: " +
                       error_name(message));
    return 0;
  }
  const char *address = nullptr;
  if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &address) < 0 ||
      !address || !*address) {
    watch->unavailable("org.a11y.Bus returned no address");
    return 0;
  }
  watch->publish(StatusReport{StatusState::on, {}, address});
  return 0;
}

} // namespace layerc::editor::a11y::atspi
