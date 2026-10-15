#include "dbus_shell_service.hpp"

#include <cerrno>
#include <string_view>
#include <utility>

namespace layerc::platform {
namespace {

constexpr std::uint32_t request_name_do_not_queue = 4;
constexpr std::uint32_t request_name_primary_owner = 1;
constexpr std::uint32_t request_name_already_owner = 4;

[[nodiscard]] std::size_t index_of(const WindowReportSource source) noexcept {
  return source == WindowReportSource::kwin ? 0U : 1U;
}

} // namespace

const char *window_report_bus_name(const WindowReportSource source) noexcept {
  return source == WindowReportSource::kwin ? kwin_bus_name : gnome_shell_bus_name;
}

ShellService::~ShellService() { stop(); }

int ShellService::read_version(sd_bus *, const char *, const char *, const char *,
                               sd_bus_message *reply, void *, sd_bus_error *) {
  return sd_bus_message_append(reply, "u", std::uint32_t{1});
}

int ShellService::handle_report(sd_bus_message *message, void *userdata, sd_bus_error *error) {
  // No exception may unwind through sd-bus; a failed report becomes a D-Bus error.
  try {
    return static_cast<ShellService *>(userdata)->report(message, error);
  } catch (...) {
    sd_bus_error_set_const(error, SD_BUS_ERROR_NO_MEMORY, "window report failed");
    return -ENOMEM;
  }
}

void ShellService::start() {
  if (running() || bus_->raw() == nullptr) return;
  guard_ = make_bus_guard();
  bus_->watch_name(kwin_bus_name);
  bus_->watch_name(gnome_shell_bus_name);
  // sd-bus keeps a pointer to this table for the slot's lifetime.
  static const sd_bus_vtable vtable[] = {
      SD_BUS_VTABLE_START(0),
      SD_BUS_PROPERTY("Version", "u", read_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
      SD_BUS_METHOD("ReportWindows", "s", "", handle_report, SD_BUS_VTABLE_UNPRIVILEGED),
      SD_BUS_VTABLE_END};
  sd_bus_slot *slot{};
  if (sd_bus_add_object_vtable(bus_->raw(), &slot, layerc_bus_path, layerc_shell_interface, vtable,
                               this) < 0) {
    state_ = NameState::failed;
    changed_ = true;
    return;
  }
  object_.reset(slot);
  state_ = NameState::requesting;
  const bool queued = bus_->call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      [](sd_bus_message *call) {
        return sd_bus_message_append(call, "su", layerc_bus_name, request_name_do_not_queue);
      },
      [this](sd_bus_message *reply, const sd_bus_error *error) {
        std::uint32_t result{};
        if (error != nullptr || sd_bus_message_read(reply, "u", &result) < 0)
          state_ = NameState::failed;
        else if (result == request_name_primary_owner || result == request_name_already_owner)
          state_ = NameState::owned;
        else
          state_ = NameState::taken;
        changed_ = true;
      },
      guard_);
  if (!queued) state_ = NameState::failed;
  changed_ = true;
}

void ShellService::stop() noexcept {
  if (!running()) return;
  // Also release a pending RequestName: bus ordering guarantees it is
  // processed first, even if its guarded callback has been cancelled.
  if (bus_->raw() != nullptr) {
    try {
      (void)bus_->call(
          "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
          [](sd_bus_message *call) { return sd_bus_message_append(call, "s", layerc_bus_name); },
          [](sd_bus_message *, const sd_bus_error *) {}, guard_);
    } catch (...) {
    }
  }
  object_.reset();
  bus_->release(guard_);
  state_ = NameState::idle;
  reports_ = {};
  changed_ = true;
}

std::string ShellService::reason() const {
  if (bus_->raw() == nullptr)
    return bus_->reason().empty() ? std::string{"bus_unavailable"} : bus_->reason();
  switch (state_) {
  case NameState::taken:
    return "bus_name_taken";
  case NameState::failed:
    return "bus_name_failed";
  default:
    return {};
  }
}

int ShellService::report(sd_bus_message *message, sd_bus_error *error) {
  const char *sender = sd_bus_message_get_sender(message);
  std::optional<WindowReportSource> source;
  for (const auto candidate : {WindowReportSource::kwin, WindowReportSource::gnome}) {
    const auto owner = bus_->owner(window_report_bus_name(candidate));
    if (sender != nullptr && owner && *owner == sender) source = candidate;
  }
  if (!source) {
    ++counters_.denied;
    sd_bus_error_set_const(error, SD_BUS_ERROR_ACCESS_DENIED,
                           "only the compositor shell may report windows");
    return -EACCES;
  }
  const char *payload{};
  if (sd_bus_message_read(message, "s", &payload) < 0 || payload == nullptr) {
    sd_bus_error_set_const(error, SD_BUS_ERROR_INVALID_ARGS, "expected one string");
    return -EINVAL;
  }
  auto &slot = reports_[index_of(*source)];
  auto parsed = parse_window_report(payload, *source);
  changed_ = true;
  if (auto *failure = std::get_if<WindowReportError>(&parsed)) {
    ++counters_.rejected;
    slot.error = failure->reason;
    sd_bus_error_set_const(error, SD_BUS_ERROR_INVALID_ARGS, "window report rejected");
    return -EINVAL;
  }
  // Prepare every allocation before committing, so a failure leaves the
  // previous report intact.
  std::string reporter{sender};
  ++counters_.accepted;
  slot.scan = std::move(std::get<WindowScan>(parsed));
  slot.reporter = std::move(reporter);
  slot.error.clear();
  return sd_bus_reply_method_return(message, "");
}

std::optional<WindowScan> ShellService::windows(const WindowReportSource source) const {
  const auto &slot = reports_[index_of(source)];
  if (!slot.scan) return std::nullopt;
  const auto owner = bus_->owner(window_report_bus_name(source));
  if (!owner || *owner != slot.reporter) return std::nullopt;
  return slot.scan;
}

const std::string &ShellService::report_error(const WindowReportSource source) const noexcept {
  return reports_[index_of(source)].error;
}

bool ShellService::take_changed() noexcept { return std::exchange(changed_, false); }

} // namespace layerc::platform
