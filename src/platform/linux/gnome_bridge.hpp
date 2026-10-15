#pragma once

#include "dbus_connection.hpp"
#include "gnome_bridge_frame.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace layerc::platform {

inline constexpr const char *gnome_bridge_bus_name = "io.github.EnderKeera.LayerC.GnomeShell";
inline constexpr const char *gnome_bridge_bus_path = "/io/github/EnderKeera/LayerC/GnomeShell";
inline constexpr const char *gnome_bridge_interface = "io.github.EnderKeera.LayerC.Overlay1";

// Transport only; the daemon's backend collects panels into full replacement
// frames. One outstanding Present, with at most one latest replacement kept.
// No threads, polling, calls to unverified owners or implicit bus connection.
class GnomeBridge {
public:
  explicit GnomeBridge(BusConnection &bus) : bus_(&bus) {}
  ~GnomeBridge();
  GnomeBridge(const GnomeBridge &) = delete;
  GnomeBridge &operator=(const GnomeBridge &) = delete;

  void set_wanted(bool wanted);
  // Called after bus dispatch to invalidate replaced/vanished owners.
  void sync();
  [[nodiscard]] bool available() const noexcept;
  // A transport fault outranks the last local frame-bounds failure.
  [[nodiscard]] const std::string &reason() const noexcept {
    return reason_.empty() ? frame_error_ : reason_;
  }
  [[nodiscard]] bool take_changed() noexcept;
  // Exact duplicate frames cause no IPC. Bounds failures remain visible but
  // do not disable transport; the next valid frame can recover.
  [[nodiscard]] bool present(std::span<const GnomePanel> panels);
  void clear();

private:
  [[nodiscard]] bool trusted() const;
  void probe(const std::string &owner);
  void send();
  void stop() noexcept;

  BusConnection *bus_;
  BusGuard guard_;
  std::string owner_;
  std::string reason_;
  // Frame validation is independent of transport readiness.
  std::string frame_error_;
  std::string desired_;
  std::string acknowledged_;
  std::uint64_t generation_{};
  bool wanted_{};
  bool verified_{};
  bool probing_{};
  bool pending_{};
  bool failed_{};
  bool changed_{};
};

} // namespace layerc::platform
