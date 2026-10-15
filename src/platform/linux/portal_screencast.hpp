#pragma once

#include "dbus_connection.hpp"
#include "pipewire_frame.hpp"
#include "portal_tokens.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace layerc::platform {

inline constexpr const char *portal_bus_name = "org.freedesktop.portal.Desktop";
inline constexpr const char *portal_path = "/org/freedesktop/portal/desktop";
inline constexpr const char *portal_screencast_interface = "org.freedesktop.portal.ScreenCast";

// The remote descriptor is owned (the descriptor in an sd-bus reply is not).
struct PortalRemote {
  int fd{-1};
  std::uint32_t node{};
  bool monitor{};
  std::optional<LogicalRect> geometry;
  PortalRemote() = default;
  ~PortalRemote();
  PortalRemote(const PortalRemote &) = delete;
  PortalRemote &operator=(const PortalRemote &) = delete;
  PortalRemote(PortalRemote &&other) noexcept;
  PortalRemote &operator=(PortalRemote &&other) noexcept;
};

// One consent session, driven entirely by BusConnection on the host loop.
// Faults are sticky until stop/start (configuration or demand change), never
// retried by sync. No PipeWire dependency reaches this transport.
class PortalScreencast {
public:
  PortalScreencast(BusConnection &bus, ScreencastTokenStore &tokens)
      : bus_(&bus), tokens_(&tokens) {}
  ~PortalScreencast();
  PortalScreencast(const PortalScreencast &) = delete;
  PortalScreencast &operator=(const PortalScreencast &) = delete;

  [[nodiscard]] bool start(std::string profile_id);
  void stop() noexcept;
  void sync();
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] const std::string &reason() const noexcept { return reason_; }
  [[nodiscard]] bool take_changed() noexcept;
  [[nodiscard]] std::optional<PortalRemote> take_remote();

private:
  enum class Step { create, select, start };
  void probe();
  void request(Step step);
  void response(Step step, sd_bus_message *message);
  void open_remote();
  void fail(std::string reason);
  void release_handles() noexcept;
  [[nodiscard]] bool accept_reply(sd_bus_message *reply, const sd_bus_error *error);
  [[nodiscard]] bool parse_results(Step step, sd_bus_message *message);
  [[nodiscard]] bool parse_streams(sd_bus_message *message);

  BusConnection *bus_;
  ScreencastTokenStore *tokens_;
  BusGuard guard_;
  BusSlot response_match_;
  BusSlot closed_match_;
  std::string profile_;
  std::string owner_;
  std::string session_path_;
  std::string request_path_;
  std::string restore_token_;
  std::string reason_;
  std::optional<PortalRemote> remote_;
  PortalRemote stream_;
  std::uint64_t generation_{};
  std::uint32_t version_{}, source_types_{}, cursor_modes_{};
  unsigned properties_{};
  bool wanted_{}, ready_{}, changed_{};
};

} // namespace layerc::platform
