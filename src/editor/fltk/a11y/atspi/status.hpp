#pragma once

#include "a11y/atspi/loop_bus.hpp"

#include <functional>
#include <string>

struct sd_bus_slot;

namespace layerc::editor::a11y::atspi {

enum class StatusState {
  // No assistive technology asked for the tree.
  off,
  // A consumer is listening on the accessibility bus.
  on,
  // Accessibility cannot be provided; reason() says why.
  unavailable
};

struct StatusReport {
  StatusState state{StatusState::off};
  std::string reason;
  // Address of the bus carrying the tree; empty while off.
  std::string address;
};

using StatusHandler = std::function<void(const StatusReport &)>;

// Watches org.a11y.Bus' org.a11y.Status on the session bus and resolves the
// accessibility bus address, exactly as the AT-SPI bridge it replaces did.
// Everything is asynchronous: an idle session bus watch is the only cost while
// no consumer is listening.
class StatusWatch {
public:
  StatusWatch();
  ~StatusWatch();
  StatusWatch(const StatusWatch &) = delete;
  StatusWatch &operator=(const StatusWatch &) = delete;

  // Starts watching (and querying) on the given connection, which must outlive
  // this object. Reports every change of the published status once.
  void start(LoopBus &session, StatusHandler report);

  [[nodiscard]] StatusReport status() const { return last_; }

private:
  static int on_properties(sd_bus_message *message, void *data,
                           sd_bus_error *error);
  static int on_owner(sd_bus_message *message, void *data, sd_bus_error *error);
  static int on_answers(sd_bus_message *message, void *data,
                        sd_bus_error *error);
  static int on_address(sd_bus_message *message, void *data,
                        sd_bus_error *error);

  void probe();
  void ask_address();
  void apply();
  void read_status_reply(sd_bus_message *message);
  void unavailable(std::string reason, std::string address = {});
  void publish(StatusReport report);

  LoopBus *session_{};
  StatusHandler report_;
  sd_bus_slot *properties_match_{};
  sd_bus_slot *owner_match_{};
  sd_bus_slot *call_slot_{};
  sd_bus_slot *address_slot_{};
  bool probing_{false};
  bool answered_{false};
  bool answering_{false};
  bool enabled_{false};
  bool screen_reader_{false};
  StatusReport last_;
};

} // namespace layerc::editor::a11y::atspi
