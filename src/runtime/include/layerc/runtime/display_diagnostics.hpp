#pragma once

#include "layerc/core/json_reader.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace layerc::runtime {

enum class DisplaySession : std::uint8_t { none, x11, wayland };
enum class DisplayBackend : std::uint8_t { auto_detect, x11, wayland, invalid };
enum class DisplayCompositor : std::uint8_t { unknown, kwin, gnome, wlroots };
enum class DisplayProviderState : std::uint8_t {
  not_built, unavailable, inactive, active, degraded,
};
inline constexpr std::array display_session_wire{
    core::EnumWireEntry{DisplaySession::none, "none"},
    core::EnumWireEntry{DisplaySession::x11, "x11"},
    core::EnumWireEntry{DisplaySession::wayland, "wayland"}};
inline constexpr std::array display_backend_wire{
    core::EnumWireEntry{DisplayBackend::auto_detect, "auto"},
    core::EnumWireEntry{DisplayBackend::x11, "x11"},
    core::EnumWireEntry{DisplayBackend::wayland, "wayland"},
    core::EnumWireEntry{DisplayBackend::invalid, "invalid"}};
inline constexpr std::array display_compositor_wire{
    core::EnumWireEntry{DisplayCompositor::unknown, "unknown"},
    core::EnumWireEntry{DisplayCompositor::kwin, "kwin"},
    core::EnumWireEntry{DisplayCompositor::gnome, "gnome"},
    core::EnumWireEntry{DisplayCompositor::wlroots, "wlroots"}};
inline constexpr std::array display_provider_state_wire{
    core::EnumWireEntry{DisplayProviderState::not_built, "not_built"},
    core::EnumWireEntry{DisplayProviderState::unavailable, "unavailable"},
    core::EnumWireEntry{DisplayProviderState::inactive, "inactive"},
    core::EnumWireEntry{DisplayProviderState::active, "active"},
    core::EnumWireEntry{DisplayProviderState::degraded, "degraded"}};

struct DisplayProviderStatus {
  std::string name{"none"};
  DisplayProviderState state{DisplayProviderState::unavailable};
  std::vector<std::string> reasons;
  bool operator==(const DisplayProviderStatus &) const = default;
};
struct DisplayProviders {
  DisplayProviderStatus windows;
  DisplayProviderStatus monitors;
  DisplayProviderStatus input;
  DisplayProviderStatus overlay;
  DisplayProviderStatus capture;
  bool operator==(const DisplayProviders &) const = default;
};
struct WaylandStatusSnapshot {
  bool connected{};
  // Highest advertised version per interface, ordered for byte-stable JSON.
  std::map<std::string, std::uint32_t, std::less<>> globals;
  std::string reason;
  bool operator==(const WaylandStatusSnapshot &) const = default;
};
struct DisplayStatusSnapshot {
  DisplaySession session{DisplaySession::none};
  DisplayBackend override_backend{DisplayBackend::auto_detect};
  std::string desktop;
  DisplayCompositor compositor{DisplayCompositor::unknown};
  WaylandStatusSnapshot wayland;
  bool bus_connected{};
  DisplayProviders providers;
  std::vector<std::string> reasons;
  bool operator==(const DisplayStatusSnapshot &) const = default;
};

} // namespace layerc::runtime
