#pragma once

#include "layerc/runtime/runtime_services.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace layerc::daemon {

// Kernel-facing adapter for one capability of a typed owner. The owner
// implements reconcile_capability, quiesce_capability, and
// capability_resources and selects the runtime holder by capability; there
// is no untyped owner pointer and no function-pointer table.
template <typename Owner>
class OwnedCapabilityService final : public layerc::runtime::IRuntimeService {
public:
  OwnedCapabilityService(Owner &owner,
                         const layerc::runtime::Capability capability) noexcept
      : owner_(owner), capability_(capability) {}

  [[nodiscard]] layerc::runtime::ServiceReconcileResult
  reconcile(const layerc::runtime::CapabilityDemand &demand) override {
    return owner_.reconcile_capability(capability_, demand);
  }
  void quiesce() noexcept override { owner_.quiesce_capability(capability_); }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return std::as_const(owner_).capability_resources(capability_);
  }

private:
  Owner &owner_;
  layerc::runtime::Capability capability_;
};

[[nodiscard]] constexpr std::array<layerc::runtime::Capability,
                                   layerc::runtime::capability_count>
every_capability() noexcept {
  std::array<layerc::runtime::Capability, layerc::runtime::capability_count>
      capabilities{};
  for (std::size_t index = 0; index < capabilities.size(); ++index)
    capabilities[index] = static_cast<layerc::runtime::Capability>(index);
  return capabilities;
}

// Owns one adapter per served capability. The adapters must outlive any
// registry they feed; capabilities the owner does not serve stay unregistered.
template <typename Owner> class OwnedCapabilityServices final {
public:
  OwnedCapabilityServices(
      Owner &owner, const std::span<const layerc::runtime::Capability> served) {
    for (const auto capability : served) {
      const auto index = static_cast<std::size_t>(capability);
      if (index < services_.size() && !services_[index])
        services_[index] =
            std::make_unique<OwnedCapabilityService<Owner>>(owner, capability);
    }
  }

  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry registry() noexcept {
    layerc::runtime::RuntimeServiceRegistry result{};
    for (std::size_t index = 0; index < services_.size(); ++index)
      result[index] = services_[index].get();
    return result;
  }

private:
  std::array<std::unique_ptr<OwnedCapabilityService<Owner>>,
             layerc::runtime::capability_count>
      services_;
};

} // namespace layerc::daemon
