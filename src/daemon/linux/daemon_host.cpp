#include "daemon_host.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace layerc::daemon {
namespace {

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

// Status is carried in a 512 KiB control frame. Bound fault detail separately
// from the stable reason token so a large set of faulty profiles stays visible
// without making the whole status response unsendable.
constexpr std::size_t max_status_diagnostics = 16;
constexpr std::size_t max_status_diagnostic_bytes = 512;

std::string status_detail(const std::string_view detail) {
  if (detail.size() <= max_status_diagnostic_bytes) return std::string(detail);
  std::size_t length = max_status_diagnostic_bytes - 3;
  while (length > 0 &&
         (static_cast<unsigned char>(detail[length]) & 0xC0U) == 0x80U)
    --length;
  return std::string(detail.substr(0, length)) + "...";
}

void resolve_availability(LinuxStatusSnapshot &linux,
                          const layerc::runtime::CapabilityDemand &demand,
                          const AvailabilityGate &gate) {
  const auto status = std::ranges::find(linux.capabilities, gate.capability,
                                        &CapabilityStatus::capability);
  if (status == linux.capabilities.end()) return;
  status->diagnostics.clear();
  for (const auto &detail : gate.session_errors) {
    if (status->diagnostics.size() == max_status_diagnostics) {
      status->diagnostics.emplace_back("additional diagnostics omitted");
      break;
    }
    status->diagnostics.push_back(status_detail(detail));
  }
  const auto set = [&](const CapabilityAvailability availability,
                       const std::string_view reason) {
    status->availability = availability;
    status->reasons = {std::string(reason)};
  };
  // Platform vs demand order differs per row (see platform_first), but the
  // remaining cascade — session health, then the secondary gate, then
  // active — is shared, so a new capability adds a table row, not a block.
  if (gate.platform_first && !gate.platform_available)
    return set(CapabilityAvailability::unavailable, "extension_missing");
  if (demand.consumers(gate.capability) == 0)
    return set(CapabilityAvailability::inactive, "not_demanded");
  // Demand-first rows only land here when demanded: isolated_visual is
  // unavailable on Linux regardless of demand, with its own token.
  if (!gate.platform_available)
    return set(CapabilityAvailability::unavailable, gate.secondary_reason);
  // The primary gate held, so a non-empty error list means individual
  // sessions failed — a degraded capability, not a platform without
  // support. "platform_unsupported" stays reserved for the
  // never-implementable rows.
  if (!gate.session_errors.empty())
    return set(CapabilityAvailability::degraded, gate.session_reason);
  if (!gate.secondary_available)
    return set(CapabilityAvailability::degraded, gate.secondary_reason);
  status->availability = CapabilityAvailability::active;
  status->reasons.clear();
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

namespace {

// VmRSS, RssAnon and Threads from /proc/self/status. The stream closes on
// return, so it is not counted among the open descriptors afterwards.
void read_proc_status(layerc::runtime::ProcessDiagnostics &process) {
  std::ifstream status("/proc/self/status");
  for (std::string line; std::getline(status, line);) {
    const auto kib = [&](const std::string_view key) -> std::optional<std::uint64_t> {
      if (!line.starts_with(key)) return std::nullopt;
      std::uint64_t value{};
      std::istringstream(line.substr(key.size())) >> value;
      return value * 1024U;
    };
    if (const auto rss = kib("VmRSS:")) process.working_set_bytes = *rss;
    if (const auto anon = kib("RssAnon:")) process.private_bytes = *anon;
    if (line.starts_with("Threads:"))
      std::istringstream(line.substr(8)) >> process.threads;
  }
}

// The shared process row from procfs and getrusage: working set is resident
// memory, private bytes is anonymous resident memory, handles are open fds.
layerc::runtime::ProcessDiagnostics process_diagnostics() {
  layerc::runtime::ProcessDiagnostics process;
  process.pid = static_cast<std::uint32_t>(::getpid());
  read_proc_status(process);
  std::error_code error;
  for (std::filesystem::directory_iterator it("/proc/self/fd", error), end;
       !error && it != end; it.increment(error))
    ++process.handles;
  // The iterator's own descriptor is counted while it is open.
  if (process.handles > 0) --process.handles;
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
    const auto ticks = [](const timeval &time) {
      return static_cast<std::uint64_t>(time.tv_sec) * 10'000'000U +
             static_cast<std::uint64_t>(time.tv_usec) * 10U;
    };
    process.kernel_time_100ns = ticks(usage.ru_stime);
    process.user_time_100ns = ticks(usage.ru_utime);
  }
  return process;
}

} // namespace

std::string status_payload_json(const layerc::runtime::DaemonKernel &kernel,
                                const LinuxStatusSnapshot &linux) {
  layerc::runtime::RuntimeDiagnostics diagnostics;
  diagnostics.process = process_diagnostics();
  diagnostics.sessions = linux.sessions;
  const auto stats = kernel.services().all_stats();
  diagnostics.services.assign(stats.begin(), stats.end());
  diagnostics.aggregate_resources = kernel.services().aggregate_resources();
  for (const auto &status : linux.capabilities)
    if (status.capability == layerc::runtime::Capability::automation)
      diagnostics.automation_faults = status.diagnostics;
  return layerc::runtime::serialize_linux_status_json(diagnostics, linux);
}

std::vector<layerc::platform::StoredProfile>
load_profiles(std::string &warning) {
  auto scan = layerc::platform::scan_profile_directory(
      layerc::platform::profile_directory());
  warning = scan.warning_text();
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
                               Client{{}, now + control_client_timeout, {}, 0});
          loop.register_fd(client.fd);
        }
      }
      continue; // one accept per wake; the level-triggered poll revisits
    }
    const auto found = clients_.find(fd);
    if (found == clients_.end()) continue;
    if (!found->second.response.empty()) {
      flush_response(loop, fd, found->second, outcome);
      continue;
    }
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
    if (reply.response.size() > layerc::platform::control_frame_limit) {
      outcome.diagnostic = "control response exceeds the frame limit";
      drop(loop, fd);
      continue;
    }
    client.response.resize(layerc::core::frame_header_bytes);
    layerc::core::append_frame_header(
        reinterpret_cast<std::byte *>(client.response.data()),
        static_cast<std::uint32_t>(reply.response.size()));
    client.response += reply.response;
    client.framer = {}; // stop accepting requests and release buffered bytes
    client.expires_at = now + control_client_timeout;
    loop.register_fd(fd, POLLOUT);
    outcome.stop_requested = outcome.stop_requested || reply.stop_requested;
    outcome.reload_requested =
        outcome.reload_requested || reply.reload_requested;
    flush_response(loop, fd, client, outcome);
  }
  return outcome;
}

void ControlService::flush_response(layerc::platform::EventLoop &loop,
                                    const int fd, Client &client,
                                    ControlOutcome &outcome) {
  // One bounded write per wake. Never wait on a slow reader in this callback;
  // the shared poll loop carries writable readiness and the absolute expiry.
  constexpr std::size_t write_budget = 64U * 1024U;
  const auto bytes = std::min(write_budget, client.response.size() - client.sent);
  const auto sent = ::send(fd, client.response.data() + client.sent, bytes,
                           MSG_NOSIGNAL | MSG_DONTWAIT);
  if (sent > 0) {
    client.sent += static_cast<std::size_t>(sent);
    if (client.sent == client.response.size()) drop(loop, fd);
  } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR)) {
    outcome.diagnostic = "control response write failed";
    drop(loop, fd);
  }
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
