#include "daemon_runtime_services.hpp"

namespace layerc::daemon {

CapabilityServices::CapabilityServices(
    void *owner, const std::span<const CapabilityPolicy> policies) {
  for (std::size_t index = 0; index < services_.size(); ++index) {
    const auto capability = static_cast<layerc::runtime::Capability>(index);
    const CapabilityPolicy *policy = nullptr;
    for (const auto &candidate : policies) {
      if (candidate.capability == capability) {
        policy = &candidate;
        break;
      }
    }
    if (policy != nullptr)
      services_[index] = std::make_unique<CapabilityService>(policy, owner);
  }
}

layerc::runtime::RuntimeServiceRegistry
CapabilityServices::registry() noexcept {
  layerc::runtime::RuntimeServiceRegistry result{};
  for (std::size_t index = 0; index < services_.size(); ++index)
    result[index] = services_[index].get();
  return result;
}

} // namespace layerc::daemon
