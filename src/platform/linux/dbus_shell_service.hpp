#pragma once

// io.github.EnderKeera.LayerC on the session bus: the Shell1 object the KWin
// script and the GNOME Shell extension report windows to (docs/Wayland.md,
// "Window report contract"). Only the current unique-name owner of
// org.kde.KWin or org.gnome.Shell may call ReportWindows.

#include "dbus_connection.hpp"
#include "dbus_window_report.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace layerc::platform {

inline constexpr const char *layerc_bus_name = "io.github.EnderKeera.LayerC";
inline constexpr const char *layerc_bus_path = "/io/github/EnderKeera/LayerC";
inline constexpr const char *layerc_shell_interface = "io.github.EnderKeera.LayerC.Shell1";
inline constexpr const char *kwin_bus_name = "org.kde.KWin";
inline constexpr const char *gnome_shell_bus_name = "org.gnome.Shell";

[[nodiscard]] const char *window_report_bus_name(WindowReportSource source) noexcept;

class ShellService {
public:
  enum class NameState : std::uint8_t { idle, requesting, owned, taken, failed };
  struct Counters {
    std::uint64_t accepted{};
    std::uint64_t rejected{};
    std::uint64_t denied{};
  };

  explicit ShellService(BusConnection &bus) : bus_(&bus) {}
  ~ShellService();
  ShellService(const ShellService &) = delete;
  ShellService &operator=(const ShellService &) = delete;

  // Exports the object, watches both reporter names, and requests the bus
  // name without queueing. Idempotent while running.
  void start();
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return state_ != NameState::idle; }
  [[nodiscard]] NameState name_state() const noexcept { return state_; }
  // bus_name_taken, bus_name_failed, or bus_unavailable; empty while owned.
  [[nodiscard]] std::string reason() const;

  // The last accepted snapshot from `source`, only while the unique name that
  // sent it still owns the source's well-known name.
  [[nodiscard]] std::optional<WindowScan> windows(WindowReportSource source) const;
  // The last rejection reason from `source`; cleared by the next accepted
  // report.
  [[nodiscard]] const std::string &report_error(WindowReportSource source) const noexcept;
  // True once after a report was accepted or rejected, or the name changed.
  [[nodiscard]] bool take_changed() noexcept;
  [[nodiscard]] Counters counters() const noexcept { return counters_; }

private:
  struct Report {
    std::optional<WindowScan> scan;
    std::string reporter;
    std::string error;
  };

  static int handle_report(sd_bus_message *message, void *userdata, sd_bus_error *error);
  static int read_version(sd_bus *, const char *, const char *, const char *,
                          sd_bus_message *reply, void *, sd_bus_error *);
  int report(sd_bus_message *message, sd_bus_error *error);

  BusConnection *bus_;
  BusGuard guard_;
  BusSlot object_;
  NameState state_{NameState::idle};
  bool changed_{};
  Counters counters_;
  std::array<Report, 2> reports_{};
};

} // namespace layerc::platform
