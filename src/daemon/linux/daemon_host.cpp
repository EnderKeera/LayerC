#include "daemon_host.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <utility>

namespace layerc::daemon {
namespace {

namespace json = layerc::core::json;

void append_resource(std::string &out, std::string_view name,
                     std::uint64_t value, json::Join &join) {
  join(out);
  json::append_quoted(out, name);
  out += ":";
  json::append_unsigned(out, value);
}

void append_x11_extension(std::string &out, std::string_view name,
                          const X11ExtensionStatus &extension,
                          std::string_view connection_reason) {
  out += ",";
  json::append_quoted(out, name);
  out += ":{\"available\":";
  json::append_bool(out, extension.available);
  if (extension.available) {
    out += ",\"major\":";
    json::append_unsigned(out, static_cast<std::uint64_t>(extension.major));
    out += ",\"minor\":";
    json::append_unsigned(out, static_cast<std::uint64_t>(extension.minor));
  } else {
    out += ",\"reason\":";
    json::append_quoted(
        out, connection_reason.empty() ? "extension_missing"
                                       : connection_reason);
  }
  out += "}";
}

// One typed gate per capability. Orchestration (demand) is separated from
// business logic (platform/session gates): each row supplies its primary
// platform gate, its session-error span with the degraded token for session
// failures, and the secondary gate that used to be a post-hoc
// degrade-after-active branch (ewmh for input, randr for overlay).
//
// platform_first keeps the historical evaluation order per row: most
// capabilities report unavailable when the platform gate fails even with no
// demand, while isolated_visual (never implementable on Linux) reports
// inactive until somebody actually demands it.
struct AvailabilityGate {
  layerc::runtime::Capability capability;
  bool platform_available;
  std::span<const std::string> session_errors;
  std::string_view session_reason;
  bool secondary_available;
  std::string_view secondary_reason;
  bool platform_first;
};

void resolve_availability(LinuxStatusSnapshot &linux,
                          const layerc::runtime::CapabilityDemand &demand,
                          const AvailabilityGate &gate) {
  const auto status = std::ranges::find(linux.capabilities, gate.capability,
                                        &CapabilityStatus::capability);
  if (status == linux.capabilities.end()) return;
  const auto unavailable = [&] {
    status->availability = CapabilityAvailability::unavailable;
    status->reasons = {"extension_missing"};
  };
  const auto inactive = [&] {
    status->availability = CapabilityAvailability::inactive;
    status->reasons = {"not_demanded"};
  };
  status->reasons.clear();
  // Platform vs demand order differs per row (see platform_first), but the
  // remaining cascade — session health, then the secondary gate, then
  // active — is shared, so a new capability adds a table row, not a block.
  if (gate.platform_first && !gate.platform_available) {
    unavailable();
    return;
  }
  if (demand.consumers(gate.capability) == 0) {
    inactive();
    return;
  }
  if (!gate.platform_available) {
    // Demand-first rows only land here when demanded: isolated_visual is
    // unavailable on Linux regardless of demand, with its own token.
    status->availability = CapabilityAvailability::unavailable;
    status->reasons = {std::string(gate.secondary_reason)};
    return;
  }
  if (!gate.session_errors.empty()) {
    // The primary gate held, so a non-empty error list means individual
    // sessions failed — a degraded capability, not a platform without
    // support. "platform_unsupported" stays reserved for the
    // never-implementable rows.
    status->availability = CapabilityAvailability::degraded;
    status->reasons = {std::string(gate.session_reason)};
    return;
  }
  if (!gate.secondary_available) {
    status->availability = CapabilityAvailability::degraded;
    status->reasons = {std::string(gate.secondary_reason)};
    return;
  }
  status->availability = CapabilityAvailability::active;
}

} // namespace

void derive_capability_status(
    LinuxStatusSnapshot &linux,
    const layerc::runtime::CapabilityDemand &demand,
    const CapabilityReasons unavailable) {
  using layerc::runtime::Capability;
  resolve_availability(linux, demand,
                       {.capability = Capability::input,
                        .platform_available = linux.x11.xi2.available,
                        .session_errors = unavailable.input,
                        .session_reason = "no_matching_device",
                        .secondary_available = linux.x11.ewmh,
                        .secondary_reason = "extension_missing",
                        .platform_first = true});
  resolve_availability(linux, demand,
                       {.capability = Capability::automation,
                        .platform_available = true,
                        .session_errors = unavailable.automation,
                        .session_reason = "profile_invalid",
                        .secondary_available = true,
                        .secondary_reason = "extension_missing",
                        .platform_first = true});
  resolve_availability(linux, demand,
                       {.capability = Capability::overlay,
                        .platform_available = linux.x11.shape.available,
                        .session_errors = unavailable.overlay,
                        .session_reason = "session_error",
                        .secondary_available = linux.x11.randr.available,
                        .secondary_reason = "extension_missing",
                        .platform_first = true});
  resolve_availability(linux, demand,
                       {.capability = Capability::visual,
                        .platform_available = true,
                        .session_errors = unavailable.visual,
                        .session_reason = "profile_invalid",
                        .secondary_available = true,
                        .secondary_reason = "extension_missing",
                        .platform_first = true});
  resolve_availability(linux, demand,
                       {.capability = Capability::isolated_visual,
                        .platform_available = false,
                        .session_errors = {},
                        .session_reason = "platform_unsupported",
                        .secondary_available = true,
                        .secondary_reason = "platform_unsupported",
                        .platform_first = false});
}

std::string status_payload_json(const layerc::runtime::DaemonKernel &kernel,
                                const LinuxStatusSnapshot &linux) {
  std::string out{"{"};
  out += "\"pid\":";
  json::append_unsigned(out, static_cast<std::uint64_t>(::getpid()));

  out += ",\"x11\":{\"connected\":";
  json::append_bool(out, linux.x11.connected);
  if (!linux.x11.reason.empty()) {
    out += ",\"reason\":";
    json::append_quoted(out, linux.x11.reason);
  }
  append_x11_extension(out, "xi2", linux.x11.xi2, linux.x11.reason);
  append_x11_extension(out, "shape", linux.x11.shape, linux.x11.reason);
  append_x11_extension(out, "randr", linux.x11.randr, linux.x11.reason);
  append_x11_extension(out, "composite", linux.x11.composite,
                       linux.x11.reason);
  append_x11_extension(out, "damage", linux.x11.damage, linux.x11.reason);
  append_x11_extension(out, "xfixes", linux.x11.xfixes, linux.x11.reason);
  out += ",\"ewmh\":{\"available\":";
  json::append_bool(out, linux.x11.ewmh);
  if (!linux.x11.ewmh) {
    out += ",\"reason\":";
    json::append_quoted(
        out, linux.x11.reason.empty() ? "extension_missing"
                                     : linux.x11.reason);
  }
  out += "}}";

  out += ",\"capabilities\":[";
  json::Join capability_join;
  for (const auto &status : linux.capabilities) {
    capability_join(out);
    out += "{\"capability\":";
    json::append_quoted(out,
                        layerc::runtime::capability_name(status.capability));
    out += ",\"availability\":";
    json::append_quoted(out, availability_name(status.availability));
    const auto stats = kernel.services().stats(status.capability);
    out += ",\"state\":";
    json::append_quoted(out, layerc::runtime::service_state_name(stats.state));
    out += ",\"consumers\":";
    json::append_unsigned(out, stats.consumers);
    out += ",\"reasons\":[";
    json::Join reason_join;
    for (const auto &reason : status.reasons) {
      reason_join(out);
      json::append_quoted(out, reason);
    }
    out += "]}";
  }
  out += "]";

  out += ",\"demand\":{";
  json::Join demand_join;
  for (std::size_t index = 0; index < layerc::runtime::capability_count;
       ++index) {
    const auto capability = static_cast<layerc::runtime::Capability>(index);
    const auto consumers = kernel.demand().consumers(capability);
    if (consumers == 0) continue;
    demand_join(out);
    json::append_quoted(out, layerc::runtime::capability_name(capability));
    out += ":";
    json::append_unsigned(out, consumers);
  }

  out += "},\"services\":[";
  const auto stats = kernel.services().all_stats();
  json::Join service_join;
  for (const auto &entry : stats) {
    if (entry.consumers == 0 &&
        entry.state == layerc::runtime::ServiceState::absent)
      continue;
    service_join(out);
    out += "{\"capability\":";
    json::append_quoted(out,
                        layerc::runtime::capability_name(entry.capability));
    out += ",\"state\":";
    json::append_quoted(
        out, layerc::runtime::service_state_name(entry.state));
    out += ",\"consumers\":";
    json::append_unsigned(out, entry.consumers);
    out += ",\"faults\":";
    json::append_unsigned(out, entry.fault_count);
    if (!entry.last_error.empty()) {
      out += ",\"error\":";
      json::append_quoted(out, entry.last_error);
    }
    out += "}";
  }

  out += "],\"resources\":{";
  const auto resources = kernel.services().aggregate_resources();
  json::Join resource_join;
  append_resource(out, "threads", resources.threads, resource_join);
  append_resource(out, "timers", resources.timers, resource_join);
  append_resource(out, "handles", resources.handles, resource_join);
  append_resource(out, "windows", resources.windows, resource_join);
  append_resource(out, "objects", resources.objects, resource_join);
  append_resource(out, "captureSessions", resources.capture_sessions,
                  resource_join);
  append_resource(out, "queues", resources.queues, resource_join);
  append_resource(out, "decodedAssets", resources.decoded_assets,
                  resource_join);
  append_resource(out, "cacheBytes", resources.cache_bytes, resource_join);
  out += "}}";
  return out;
}

std::vector<layerc::platform::StoredProfile>
load_profiles(std::string &warning) {
  auto scan = layerc::platform::scan_profile_directory(
      layerc::platform::profile_directory());
  warning = std::move(scan.warning);
  if (!scan.error.empty()) {
    if (!warning.empty()) warning += "; ";
    warning += scan.error;
    return {};
  }

  return std::move(scan.profiles);
}

ControlReply handle_control_request(
    const std::string_view request_json,
    const layerc::runtime::DaemonKernel &kernel,
    const LinuxStatusSnapshot &linux) {
  const auto command = layerc::platform::parse_control_request(request_json);
  if (!command.has_value())
    return {.response = layerc::platform::encode_control_response(
                false, "invalid LayerC.Control request", {})};
  switch (*command) {
  case layerc::platform::ControlCommand::status:
    return {.response = layerc::platform::encode_control_response(
                true, {}, status_payload_json(kernel, linux))};
  case layerc::platform::ControlCommand::reload:
  case layerc::platform::ControlCommand::notify_reload:
    return {.response = layerc::platform::encode_control_response(true, {}, {}),
            .reload_requested = true};
  case layerc::platform::ControlCommand::stop:
    return {.response = layerc::platform::encode_control_response(true, {}, {}),
            .stop_requested = true};
  }
  return {.response = layerc::platform::encode_control_response(
              false, "invalid control command", {})};
}

bool ControlService::start(std::string &error, std::string &warning) {
  auto listener = layerc::platform::ControlListener::open(error, warning);
  if (!listener.has_value()) return false;
  listener_ = std::move(*listener);
  return true;
}

void ControlService::attach(layerc::platform::EventLoop &loop) noexcept {
  loop.register_fd(listener_.fd());
}

ControlOutcome ControlService::service(
    layerc::platform::EventLoop &loop, std::vector<int> ready,
    const layerc::runtime::DaemonKernel &kernel,
    const LinuxStatusSnapshot &linux) {
  ControlOutcome outcome;
  const auto now = layerc::core::MonotonicClock::now();
  expire_clients(loop, now);
  for (const int fd : ready) {
    if (fd == listener_.fd()) {
      const auto client = listener_.accept();
      if (!client.error.empty()) outcome.diagnostic = client.error;
      if (client.state == layerc::platform::ControlAcceptResult::State::accepted) {
        if (clients_.size() >= control_client_limit) {
          (void)::close(client.fd);
          outcome.diagnostic = "control client limit reached";
        } else {
          clients_.try_emplace(client.fd,
                               Client{{}, now + control_client_timeout});
          loop.register_fd(client.fd);
        }
      }
      continue; // one accept per wake; the level-triggered poll revisits
    }
    const auto found = clients_.find(fd);
    if (found == clients_.end()) continue;
    std::array<char, 8192> chunk{};
    const ssize_t received = ::read(fd, chunk.data(), chunk.size());
    if (received == 0) {
      drop(loop, fd); // peer closed
      continue;
    }
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        drop(loop, fd);
      continue;
    }
    auto &client = found->second;
    client.expires_at = now + control_client_timeout;
    client.framer.feed(std::string_view{chunk.data(),
                                        static_cast<std::size_t>(received)});
    const auto request = client.framer.next_frame();
    if (!request.has_value()) {
      if (client.framer.fault() !=
          layerc::platform::ControlFramer::Fault::none)
        drop(loop, fd);
      continue; // partial frame
    }
    const auto reply = handle_control_request(*request, kernel, linux);
    std::string send_error;
    if (!layerc::platform::control_send_frame(fd, reply.response, send_error))
      outcome.diagnostic = std::move(send_error);
    outcome.stop_requested = outcome.stop_requested || reply.stop_requested;
    outcome.reload_requested =
        outcome.reload_requested || reply.reload_requested;
    drop(loop, fd); // one request + one response per connection
  }
  return outcome;
}

void ControlService::expire_clients(
    layerc::platform::EventLoop &loop,
    const layerc::core::TimePoint now) noexcept {
  for (auto client = clients_.begin(); client != clients_.end();) {
    if (client->second.expires_at > now) {
      ++client;
      continue;
    }
    const int fd = client->first;
    client = clients_.erase(client);
    loop.unregister_fd(fd);
    (void)::close(fd);
  }
}

std::optional<layerc::core::TimePoint>
ControlService::next_deadline() const noexcept {
  if (clients_.empty()) return std::nullopt;
  return std::ranges::min_element(clients_, {}, [](const auto &entry) {
           return entry.second.expires_at;
         })->second.expires_at;
}

void ControlService::shutdown(layerc::platform::EventLoop &loop) noexcept {
  for (const auto &client : clients_) {
    loop.unregister_fd(client.first);
    (void)::close(client.first);
  }
  clients_.clear();
  loop.unregister_fd(listener_.fd());
}

void ControlService::drop(layerc::platform::EventLoop &loop,
                          const int fd) noexcept {
  clients_.erase(fd);
  loop.unregister_fd(fd);
  (void)::close(fd);
}

} // namespace layerc::daemon
