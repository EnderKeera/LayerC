#pragma once

#include "layerc/runtime/platform_contracts.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace layerc::runtime {

struct SessionCapabilityRequest {
  WindowId window;
  bool active{};
  bool global{};
  bool visual{};
  bool templates{};
  bool generic_hid{};
  bool isolated_visual{};
  bool companion{};
};

[[nodiscard]] CapabilityDemand calculate_capability_demand(
    std::span<const SessionCapabilityRequest> sessions,
    bool editor_connected, bool edit_overlay_active) noexcept;

// Platform-neutral authority for immutable capability demand and first-party
// service ownership. Native adapters remain outside this class.
class DaemonKernel {
public:
  [[nodiscard]] bool reconcile(
      CapabilityDemand demand,
      std::span<const RuntimeServiceDescriptor> descriptors,
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
    std::vector<RuntimeServiceDescriptor> descriptors;
    core::TimePoint now;
  };

  RuntimeServiceHost services_;
  CapabilityDemand demand_;
  std::uint64_t demand_generation_{};
  bool reconciling_{};
  std::optional<PendingReconcile> pending_;
};

} // namespace layerc::runtime
