#pragma once

#include "session_registry.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>

namespace layerc::daemon {

// The automation trace subscribers LayerD serves: one lease per client, renewed
// by polls. Runtime tracing is enabled exactly for profiles with a live lease.
// Nothing here owns sockets.
class TraceLeaseHost final {
public:
  [[nodiscard]] TraceControlResult handle(const layerc::core::TraceControlRequest &request,
                                          std::span<InputSession> sessions,
                                          layerc::core::TimePoint now);
  // Drops lapsed leases and re-applies demand; true when any runtime's tracing
  // changed.
  bool expire(layerc::core::TimePoint now, std::span<InputSession> sessions);
  // Re-applies demand after sessions are rebuilt (new runtimes start untraced).
  void reconcile(std::span<InputSession> sessions);
  // The earliest lease expiry, so the engine's deadline covers trace leases.
  [[nodiscard]] std::optional<layerc::core::TimePoint> next_deadline() const noexcept;

private:
  struct Lease {
    std::string profile_id;
    layerc::core::TimePoint expires;
    std::uint64_t generation{};
    std::uint64_t cursor{};       // last sequence delivered
    std::uint64_t acknowledged{}; // last cursor the client sent back
  };
  [[nodiscard]] static bool demanded(const std::map<std::string, Lease, std::less<>> &leases,
                                     const std::string &profile_id);
  std::map<std::string, Lease, std::less<>> leases_;
};

} // namespace layerc::daemon
