#include "daemon_runtime_services.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using layerc::daemon::every_capability;
using layerc::daemon::OwnedCapabilityServices;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileResult;
using layerc::test::expect;

constexpr std::size_t slot(const Capability capability) noexcept {
  return static_cast<std::size_t>(capability);
}

// Records every typed dispatch so the adapters can be checked for routing.
struct FakeOwner {
  std::array<ServiceReconcileResult, layerc::runtime::capability_count> results{};
  std::array<RuntimeResourceStats, layerc::runtime::capability_count> owned{};
  std::array<unsigned, layerc::runtime::capability_count> reconcile_calls{};
  std::array<unsigned, layerc::runtime::capability_count> quiesce_calls{};
  mutable std::array<unsigned, layerc::runtime::capability_count> resource_calls{};
  std::array<std::uint32_t, layerc::runtime::capability_count>
      observed_consumers{};
  mutable Capability last_capability{Capability::count};

  ServiceReconcileResult reconcile_capability(const Capability capability,
                                              const CapabilityDemand &demand) {
    ++reconcile_calls[slot(capability)];
    observed_consumers[slot(capability)] = demand.consumers(capability);
    last_capability = capability;
    return results[slot(capability)];
  }
  void quiesce_capability(const Capability capability) noexcept {
    ++quiesce_calls[slot(capability)];
    last_capability = capability;
  }
  RuntimeResourceStats
  capability_resources(const Capability capability) const noexcept {
    ++resource_calls[slot(capability)];
    last_capability = capability;
    return owned[slot(capability)];
  }
};

void adapters_preserve_capability_identity_and_results() {
  FakeOwner owner;
  OwnedCapabilityServices<FakeOwner> services(owner, every_capability());
  const auto registry = services.registry();

  for (std::size_t index = 0; index < registry.size(); ++index) {
    const auto capability = static_cast<Capability>(index);
    auto *service = registry[index];
    expect(service != nullptr, "every runtime capability should have an adapter");
    if (service == nullptr)
      continue;
    expect(service->capability() == capability,
           "adapter capability should match its registry slot");

    CapabilityDemand demand;
    demand.set(capability, static_cast<std::uint32_t>(index + 1U));
    owner.results[index] =
        index == slot(Capability::visual)
            ? ServiceReconcileResult{false, "capture unavailable"}
            : ServiceReconcileResult{};
    owner.owned[index] = {.handles = static_cast<std::uint32_t>(index + 10U),
                          .objects = static_cast<std::uint32_t>(index + 20U)};

    const auto reconciled = service->reconcile(demand);
    expect(reconciled.success == owner.results[index].success &&
               reconciled.error == owner.results[index].error,
           "adapter should preserve typed reconcile results");
    expect(owner.last_capability == capability &&
               owner.observed_consumers[index] == index + 1U,
           "adapter should route demand to the matching capability");
    expect(service->resources() == owner.owned[index],
           "adapter should report only its capability's owned resources");
    service->quiesce();
    expect(owner.last_capability == capability,
           "adapter should quiesce the matching capability");
  }

  for (std::size_t index = 0; index < registry.size(); ++index) {
    expect(owner.reconcile_calls[index] == 1 &&
               owner.resource_calls[index] == 1 &&
               owner.quiesce_calls[index] == 1,
           "each adapter operation should reach the owner exactly once");
  }
}

void unserved_capability_stays_unregistered() {
  FakeOwner owner;
  const std::array<Capability, 1> served{{Capability::input}};
  OwnedCapabilityServices<FakeOwner> services(owner, served);
  const auto registry = services.registry();
  expect(registry[slot(Capability::input)] != nullptr,
         "a served capability should be registered");
  for (std::size_t index = 1; index < registry.size(); ++index)
    expect(registry[index] == nullptr,
           "an unserved capability should stay unregistered");
}

} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(adapters_preserve_capability_identity_and_results),
    LAYERC_TEST_CASE(unserved_capability_stays_unregistered))
