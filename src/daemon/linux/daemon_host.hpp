#pragma once

#include "control_channel.hpp"
#include "event_loop.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/automation_trace_control.hpp"
#include "layerc/core/live_draft_control.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"
#include "layerc/runtime/diagnostics.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {

// Capabilities that are demanded but have no registered runtime service.
// The Linux host reports these so operators can see why, for example, visual
// recognition stays offline on a machine without the native adapter.
[[nodiscard]] inline std::vector<std::string_view> unavailable_capabilities(
    const layerc::runtime::CapabilityDemand &demand,
    const layerc::runtime::RuntimeServiceHost &services) noexcept {
  std::vector<std::string_view> result;
  for (std::size_t index = 0; index < layerc::runtime::capability_count;
       ++index) {
    const auto capability = static_cast<layerc::runtime::Capability>(index);
    if (demand.consumers(capability) == 0) continue;
    if (services.find(capability) != nullptr) continue;
    result.push_back(layerc::runtime::capability_name(capability));
  }
  return result;
}

using layerc::runtime::X11ExtensionStatus;
using layerc::runtime::X11StatusSnapshot;
using layerc::runtime::CapabilityAvailability;
using layerc::runtime::CapabilityStatus;
using layerc::runtime::LinuxStatusSnapshot;
using layerc::runtime::availability_name;

struct CapabilityReasons {
  std::span<const std::string> input;
  std::span<const std::string> overlay;
  std::span<const std::string> automation;
  std::span<const std::string> visual;
  // A native (Wayland layer-shell) overlay backend replaces X11 Shape/RandR.
  bool native_overlay{};
  // The opted-in evdev source replaces XI2 (and needs no X11 window ids).
  bool native_input{};
};

void derive_capability_status(
    LinuxStatusSnapshot &linux,
    const layerc::runtime::CapabilityDemand &demand,
    CapabilityReasons unavailable);

// Byte-stable JSON snapshot of kernel state and Linux/X11 capability probes.
[[nodiscard]] std::string status_payload_json(
    const layerc::runtime::DaemonKernel &kernel,
    const LinuxStatusSnapshot &linux = {});

// Every profile in the XDG store that parses. Unparseable files are skipped
// and reported in `warning` so the daemon keeps serving the rest.
[[nodiscard]] std::vector<layerc::platform::StoredProfile>
load_profiles(std::string &warning);

struct ControlReply {
  std::string response;
  bool stop_requested{};
  bool reload_requested{};
};

using TraceControlResult =
    std::variant<layerc::core::TraceControlReply, layerc::core::TraceControlFailure>;
using TraceControlHandler = std::function<TraceControlResult(const layerc::core::TraceControlRequest &)>;
using DraftControlResult =
    std::variant<layerc::core::LiveDraftReply, layerc::core::LiveDraftFailure>;
using DraftControlHandler = std::function<DraftControlResult(const layerc::core::LiveDraftRequest &)>;

// LayerC.Control v1 dispatch; trace and draft payloads carry their own typed
// versions.
[[nodiscard]] ControlReply handle_control_request(
    std::string_view request_json,
    const layerc::runtime::DaemonKernel &kernel,
    const LinuxStatusSnapshot &linux = {},
    const TraceControlHandler &trace = {},
    const DraftControlHandler &draft = {});

struct ControlOutcome {
  std::string diagnostic;
  bool stop_requested{};
  bool reload_requested{};
};

// Owns the control listener plus per-connection framer state; one request
// and one response per connection, then the socket closes.
inline constexpr std::size_t control_client_limit = 16;
inline constexpr layerc::core::Milliseconds control_client_timeout{5000};

class ControlService {
public:
  [[nodiscard]] bool start(std::string &error, std::string &warning);
  void attach(layerc::platform::EventLoop &loop) noexcept;

  // Processes every descriptor that woke the loop (value copy at the
  // boundary; safe to unregister during iteration).
  [[nodiscard]] ControlOutcome service(
      layerc::platform::EventLoop &loop, std::vector<int> ready,
      const layerc::runtime::DaemonKernel &kernel,
      const LinuxStatusSnapshot &linux = {},
      const TraceControlHandler &trace = {},
      const DraftControlHandler &draft = {});

  void expire_clients(
      layerc::platform::EventLoop &loop,
      layerc::core::TimePoint now = layerc::core::MonotonicClock::now()) noexcept;
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline() const noexcept;
  void shutdown(layerc::platform::EventLoop &loop) noexcept;

  [[nodiscard]] int listener_fd() const noexcept { return listener_.fd(); }
  [[nodiscard]] std::size_t client_count() const noexcept {
    return clients_.size();
  }

private:
  void drop(layerc::platform::EventLoop &loop, int fd) noexcept;

  struct Client {
    layerc::core::FrameDecoder framer{layerc::platform::control_frame_limit,
                                      layerc::platform::control_buffer_limit};
    layerc::core::TimePoint expires_at;
    std::string response;
    std::size_t sent{};
  };

  void flush_response(layerc::platform::EventLoop &loop, int fd,
                      Client &client, ControlOutcome &outcome);

  layerc::platform::ControlListener listener_;
  std::map<int, Client> clients_{};
};

} // namespace layerc::daemon
