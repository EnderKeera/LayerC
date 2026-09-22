#pragma once

#include "layerc/runtime/runtime_services.hpp"

#include <array>
#include <memory>
#include <span>

namespace layerc::daemon {

// The only dispatch table between DaemonKernel and daemon-owned service
// methods. One entry per capability; owned by DaemonApp and registered once.
struct CapabilityPolicy {
  layerc::runtime::Capability capability;
  layerc::runtime::ServiceReconcileResult (*reconcile)(
      void *owner, const layerc::runtime::CapabilityDemand &demand);
  void (*quiesce)(void *owner) noexcept;
  layerc::runtime::RuntimeResourceStats (*resources)(
      const void *owner) noexcept;
};

// Single IRuntimeService implementation: capability slot → policy → owner.
// This is the only virtual hop between DaemonKernel and platform methods.
class CapabilityService final : public layerc::runtime::IRuntimeService {
public:
  CapabilityService(const CapabilityPolicy *policy, void *owner) noexcept
      : policy_(policy), owner_(owner) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return policy_->capability;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override {
    return policy_->reconcile(owner_, demand);
  }

  void quiesce() noexcept override { policy_->quiesce(owner_); }

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return policy_->resources(owner_);
  }

private:
  const CapabilityPolicy *policy_;
  void *owner_;
};

// Owns one CapabilityService per policy entry. The services must outlive any
// registry they feed.
class CapabilityServices final {
public:
  CapabilityServices(void *owner, std::span<const CapabilityPolicy> policies);

  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry registry() noexcept;

private:
  std::array<std::unique_ptr<layerc::runtime::IRuntimeService>,
             layerc::runtime::capability_count>
      services_;
};

} // namespace layerc::daemon
