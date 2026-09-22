#include "daemon_runtime_services.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using layerc::daemon::CapabilityPolicy;
using layerc::daemon::CapabilityService;
using layerc::daemon::CapabilityServices;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::ServiceReconcileResult;
using layerc::test::expect;

constexpr std::size_t slot(const Capability capability) noexcept {
  return static_cast<std::size_t>(capability);
}

struct FakeOwner {
  std::array<ServiceReconcileResult, layerc::runtime::capability_count> results{};
  std::array<RuntimeResourceStats, layerc::runtime::capability_count> owned{};
  std::array<unsigned, layerc::runtime::capability_count> reconcile_calls{};
  std::array<unsigned, layerc::runtime::capability_count> quiesce_calls{};
  mutable std::array<unsigned, layerc::runtime::capability_count> resource_calls{};
  std::array<std::uint32_t, layerc::runtime::capability_count>
      observed_consumers{};
  mutable Capability last_capability{Capability::count};
};

std::span<const CapabilityPolicy> fake_policies() {
  static constexpr std::array<CapabilityPolicy,
                              layerc::runtime::capability_count>
      policies{{
          {Capability::input,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::input)];
             self.observed_consumers[slot(Capability::input)] =
                 demand.consumers(Capability::input);
             self.last_capability = Capability::input;
             return self.results[slot(Capability::input)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::input)];
             self.last_capability = Capability::input;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::input)];
             self.last_capability = Capability::input;
             return self.owned[slot(Capability::input)];
           }},
          {Capability::generic_hid,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::generic_hid)];
             self.observed_consumers[slot(Capability::generic_hid)] =
                 demand.consumers(Capability::generic_hid);
             self.last_capability = Capability::generic_hid;
             return self.results[slot(Capability::generic_hid)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::generic_hid)];
             self.last_capability = Capability::generic_hid;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::generic_hid)];
             self.last_capability = Capability::generic_hid;
             return self.owned[slot(Capability::generic_hid)];
           }},
          {Capability::overlay,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::overlay)];
             self.observed_consumers[slot(Capability::overlay)] =
                 demand.consumers(Capability::overlay);
             self.last_capability = Capability::overlay;
             return self.results[slot(Capability::overlay)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::overlay)];
             self.last_capability = Capability::overlay;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::overlay)];
             self.last_capability = Capability::overlay;
             return self.owned[slot(Capability::overlay)];
           }},
          {Capability::visual,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::visual)];
             self.observed_consumers[slot(Capability::visual)] =
                 demand.consumers(Capability::visual);
             self.last_capability = Capability::visual;
             return self.results[slot(Capability::visual)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::visual)];
             self.last_capability = Capability::visual;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::visual)];
             self.last_capability = Capability::visual;
             return self.owned[slot(Capability::visual)];
           }},
          {Capability::isolated_visual,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::isolated_visual)];
             self.observed_consumers[slot(Capability::isolated_visual)] =
                 demand.consumers(Capability::isolated_visual);
             self.last_capability = Capability::isolated_visual;
             return self.results[slot(Capability::isolated_visual)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::isolated_visual)];
             self.last_capability = Capability::isolated_visual;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::isolated_visual)];
             self.last_capability = Capability::isolated_visual;
             return self.owned[slot(Capability::isolated_visual)];
           }},
          {Capability::automation,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::automation)];
             self.observed_consumers[slot(Capability::automation)] =
                 demand.consumers(Capability::automation);
             self.last_capability = Capability::automation;
             return self.results[slot(Capability::automation)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::automation)];
             self.last_capability = Capability::automation;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::automation)];
             self.last_capability = Capability::automation;
             return self.owned[slot(Capability::automation)];
           }},
          {Capability::gamebar,
           [](void *owner, const CapabilityDemand &demand) {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.reconcile_calls[slot(Capability::gamebar)];
             self.observed_consumers[slot(Capability::gamebar)] =
                 demand.consumers(Capability::gamebar);
             self.last_capability = Capability::gamebar;
             return self.results[slot(Capability::gamebar)];
           },
           [](void *owner) noexcept {
             auto &self = *static_cast<FakeOwner *>(owner);
             ++self.quiesce_calls[slot(Capability::gamebar)];
             self.last_capability = Capability::gamebar;
           },
           [](const void *owner) noexcept {
             auto &self = *static_cast<const FakeOwner *>(owner);
             ++self.resource_calls[slot(Capability::gamebar)];
             self.last_capability = Capability::gamebar;
             return self.owned[slot(Capability::gamebar)];
           }},
      }};
  return policies;
}

void adapters_preserve_capability_identity_and_results() {
  FakeOwner owner;
  CapabilityServices services(&owner, fake_policies());
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
           "each adapter operation should cross the policy exactly once");
  }
}

void missing_policy_leaves_slot_unregistered() {
  FakeOwner owner;
  const std::array<CapabilityPolicy, 1> partial{{
      {Capability::input,
       [](void *, const CapabilityDemand &) { return ServiceReconcileResult{}; },
       [](void *) noexcept {},
       [](const void *) noexcept { return RuntimeResourceStats{}; }},
  }};
  CapabilityServices services(&owner, partial);
  const auto registry = services.registry();
  expect(registry[slot(Capability::input)] != nullptr,
         "a provided policy should register its capability");
  for (std::size_t index = 1; index < registry.size(); ++index)
    expect(registry[index] == nullptr,
           "a missing policy should leave its capability unregistered");
}

} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(adapters_preserve_capability_identity_and_results),
    LAYERC_TEST_CASE(missing_policy_leaves_slot_unregistered))
