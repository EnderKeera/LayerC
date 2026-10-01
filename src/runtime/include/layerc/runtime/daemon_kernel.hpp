#pragma once

#include "layerc/runtime/runtime_services.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace layerc::runtime {

// Platform code creates WindowIds and keeps native handles in its adapter.
// Runtime code may compare or persist an ID only for the lifetime of a scan.
// The struct itself lives in core (a leaf handle, not kernel policy) so the
// capture layer does not link the runtime for it.
using WindowId = core::WindowId;

struct SessionCapabilityRequest {
  WindowId window;
  bool active{};
  bool global{};
  // An active session can be driven only by automation or visual sampling.
  // Defaults preserve callers that represent a full desktop session.
  bool input{true};
  bool overlay{true};
  bool visual{};
  bool generic_hid{};
  bool isolated_visual{};
  bool automation{};
  bool companion{};
  std::string profile_id;
};

[[nodiscard]] CapabilityDemand
calculate_capability_demand(std::span<const SessionCapabilityRequest> sessions,
                            bool edit_overlay_active);

// Platform-neutral authority for immutable capability demand and first-party
// service ownership. Native adapters remain outside this class.
class DaemonKernel {
public:
  explicit DaemonKernel(RuntimeServiceRegistry services = {}) noexcept;

  [[nodiscard]] bool
  reconcile(CapabilityDemand demand,
            core::TimePoint now = core::MonotonicClock::now());
  void quiesce(core::TimePoint now = core::MonotonicClock::now()) noexcept;

  [[nodiscard]] const CapabilityDemand &demand() const noexcept {
    return demand_;
  }
  [[nodiscard]] std::uint64_t demand_generation() const noexcept {
    return demand_generation_;
  }
  [[nodiscard]] RuntimeServiceHost &services() noexcept { return services_; }
  [[nodiscard]] const RuntimeServiceHost &services() const noexcept {
    return services_;
  }

private:
  struct PendingReconcile {
    CapabilityDemand demand;
    core::TimePoint now;
  };

  RuntimeServiceHost services_;
  CapabilityDemand demand_;
  std::uint64_t demand_generation_{};
  bool reconciling_{};
  std::optional<PendingReconcile> pending_;
};

} // namespace layerc::runtime
