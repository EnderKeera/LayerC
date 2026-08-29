#include "layerc/runtime/daemon_kernel.hpp"
#include "test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using layerc::test::expect;
using layerc::runtime::Capability;
using layerc::runtime::CapabilityDemand;
using layerc::runtime::RuntimeResourceStats;
using layerc::runtime::RuntimeServiceDescriptor;
using layerc::runtime::ServiceReconcileResult;

struct FakeContext {
  Capability capability{Capability::visual};
  int reconciled{};
  int quiesced{};
  bool fail{};
  std::vector<Capability> *quiesce_order{};
};

ServiceReconcileResult fake_reconcile(void *const context) {
  auto &fake = *static_cast<FakeContext *>(context);
  ++fake.reconciled;
  return fake.fail ? ServiceReconcileResult{false, "test fault"}
                   : ServiceReconcileResult{};
}

void fake_quiesce(void *const context) noexcept {
  auto &fake = *static_cast<FakeContext *>(context);
  ++fake.quiesced;
  if (fake.quiesce_order != nullptr)
    fake.quiesce_order->push_back(fake.capability);
}

RuntimeResourceStats fake_resources(void *) noexcept {
  return {.threads = 1, .capture_sessions = 1};
}

[[nodiscard]] RuntimeServiceDescriptor fake_descriptor(
    FakeContext &context, const Capability capability) {
  context.capability = capability;
  return {.capability = capability,
          .context = &context,
          .reconcile = &fake_reconcile,
          .quiesce = &fake_quiesce,
          .resources = &fake_resources};
}

void demand_counts_saturate() {
  CapabilityDemand demand;
  demand.add(Capability::input, 2);
  demand.add(Capability::input, 3);
  expect(demand.consumers(Capability::input) == 5,
         "capability consumers should accumulate");
  expect(demand.required(Capability::input),
         "non-zero demand should be required");
  expect(!demand.empty(), "non-zero demand should not be empty");
  demand.set(Capability::input, 0);
  expect(demand.empty(), "cleared demand should be empty");
}

void host_is_lazy_and_idempotent() {
  FakeContext context;
  const auto descriptor = fake_descriptor(context, Capability::visual);
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;

  expect(host.reconcile(demand, {&descriptor, 1}),
         "empty demand should reconcile");
  expect(context.reconciled == 0, "empty demand must not run a service");

  demand.set(Capability::visual, 2);
  expect(host.reconcile(demand, {&descriptor, 1}),
         "active demand should reconcile");
  expect(context.reconciled == 1, "first demand should reconcile once");

  expect(host.reconcile(demand, {&descriptor, 1}),
         "repeated demand should reconcile");
  expect(context.reconciled == 2 &&
             host.stats(Capability::visual).activation_count == 1,
         "repeated demand must reuse the service");

  const auto stats = host.stats(Capability::visual);
  expect(stats.state == layerc::runtime::ServiceState::active &&
             stats.consumers == 2 && stats.resources.threads == 1,
         "active statistics should reflect service resources");

  demand.set(Capability::visual, 0);
  expect(host.reconcile(demand, {&descriptor, 1}),
         "zero demand should quiesce successfully");
  expect(context.quiesced == 1 && host.find(Capability::visual) == nullptr,
         "zero demand must quiesce and destroy the service");
  expect(host.stats(Capability::visual).state ==
             layerc::runtime::ServiceState::absent,
         "quiesced service should report absent");
}

void faults_release_the_service() {
  FakeContext context;
  context.fail = true;
  const auto descriptor = fake_descriptor(context, Capability::visual);
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;
  demand.set(Capability::visual, 1);

  expect(!host.reconcile(demand, {&descriptor, 1}),
         "service failure should fail reconciliation");
  const auto stats = host.stats(Capability::visual);
  expect(context.quiesced == 1 && host.find(Capability::visual) == nullptr,
         "failed service should be quiesced and destroyed");
  expect(stats.state == layerc::runtime::ServiceState::faulted &&
             stats.fault_count == 1 && stats.last_error == "test fault",
         "failure diagnostics should be retained");
}

void incomplete_descriptor_faults() {
  FakeContext context;
  const auto descriptor = RuntimeServiceDescriptor{
      .capability = Capability::visual,
      .context = &context,
      .reconcile = &fake_reconcile,
      .quiesce = nullptr,
      .resources = &fake_resources};
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;
  demand.set(Capability::visual, 1);

  expect(!host.reconcile(demand, {&descriptor, 1}),
         "an incomplete descriptor should fail reconciliation");
  expect(context.reconciled == 0 && context.quiesced == 0,
         "an incomplete descriptor must not run any callback");
  const auto stats = host.stats(Capability::visual);
  expect(stats.state == layerc::runtime::ServiceState::faulted &&
             stats.last_error == "required service descriptor is incomplete",
         "incomplete descriptor should be diagnosable");
}

void later_fault_rolls_back_new_dependencies() {
  FakeContext input;
  FakeContext visual;
  visual.fail = true;
  const RuntimeServiceDescriptor descriptors[] = {
      fake_descriptor(input, Capability::input),
      fake_descriptor(visual, Capability::visual)};
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;
  demand.set(Capability::input, 1);
  demand.set(Capability::visual, 1);

  expect(!host.reconcile(demand, descriptors),
         "a later service fault should fail the capability transition");
  expect(input.quiesced == 1 && visual.quiesced == 1 &&
             host.find(Capability::input) == nullptr &&
             host.find(Capability::visual) == nullptr,
         "newly activated dependencies should roll back with the failed tier");
  expect(host.stats(Capability::visual).state ==
                 layerc::runtime::ServiceState::faulted &&
             host.stats(Capability::input).state ==
                 layerc::runtime::ServiceState::absent,
         "the root fault should remain diagnosable after dependency rollback");

  visual.fail = false;
  expect(host.reconcile(demand, descriptors) &&
             host.stats(Capability::input).activation_count == 2 &&
             host.stats(Capability::visual).activation_count == 2,
         "a corrected demand should retry every rolled-back capability");
}

void teardown_is_reverse_ordered() {
  std::vector<Capability> order;
  FakeContext input;
  input.quiesce_order = &order;
  FakeContext visual;
  visual.quiesce_order = &order;
  const RuntimeServiceDescriptor descriptors[] = {
      fake_descriptor(input, Capability::input),
      fake_descriptor(visual, Capability::visual)};
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;
  demand.set(Capability::input, 1);
  demand.set(Capability::visual, 1);
  expect(host.reconcile(demand, descriptors), "multiple services should activate");
  demand = {};
  expect(host.reconcile(demand, descriptors), "multiple services should quiesce");
  expect(order.size() == 2 && order[0] == Capability::visual &&
             order[1] == Capability::input,
         "services should quiesce in reverse dependency order");
}

void repeated_activation_has_bounded_ownership() {
  FakeContext context;
  const auto descriptor = fake_descriptor(context, Capability::visual);
  layerc::runtime::RuntimeServiceHost host;
  CapabilityDemand demand;
  for (int cycle = 0; cycle < 100; ++cycle) {
    demand.set(Capability::visual, 1);
    expect(host.reconcile(demand, {&descriptor, 1}),
           "activation cycle should succeed");
    demand.set(Capability::visual, 0);
    expect(host.reconcile(demand, {&descriptor, 1}),
           "quiescence cycle should succeed");
  }
  const auto stats = host.stats(Capability::visual);
  expect(context.reconciled == 100 && context.quiesced == 100,
         "every activation should have one matching quiescence");
  expect(stats.activation_count == 100 &&
             stats.state == layerc::runtime::ServiceState::absent &&
             stats.resources == RuntimeResourceStats{},
         "inactive lifecycle statistics should retain counts but no resources");
}

void demand_matrix_is_explicit() {
  using layerc::runtime::SessionCapabilityRequest;
  const auto calculate = [](std::initializer_list<SessionCapabilityRequest> s,
                            const bool editor = false,
                            const bool edit = false) {
    return layerc::runtime::calculate_capability_demand({s.begin(), s.size()},
                                                        editor, edit);
  };

  expect(calculate({}).empty(), "no game should demand no service");
  expect(calculate({{.window = {1}, .active = false}}).empty(),
         "a background non-global game should demand no service");

  auto demand = calculate({{.window = {1}, .active = true}});
  expect(demand.consumers(Capability::input) == 1 &&
             demand.consumers(Capability::overlay) == 1 &&
             !demand.required(Capability::visual),
         "input-only foreground game should own input and overlay only");

  demand = calculate({{.window = {1},
                       .active = true,
                       .global = true,
                       .visual = true,
                       .templates = true}});
  expect(demand.required(Capability::input) &&
             !demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "global profiles should not activate capture");

  demand = calculate({{.window = {1}, .active = true, .visual = true}});
  expect(demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "pixel observations should activate inline visual only");

  demand = calculate({{.window = {1},
                       .active = true,
                       .visual = true,
                       .templates = true,
                       .generic_hid = true}});
  expect(demand.required(Capability::visual) &&
             demand.required(Capability::template_evaluation) &&
             demand.required(Capability::generic_hid),
         "template and HID consumers should activate their lazy children");

  demand = calculate({
      {.window = {9}, .active = true, .visual = true, .templates = true},
      {.window = {9}, .active = true, .visual = true, .isolated_visual = true},
  });
  expect(demand.consumers(Capability::isolated_visual) == 2 &&
             !demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "one isolated profile should route every profile on its HWND through "
         "one isolated tier");

  demand = calculate({}, true, false);
  expect(demand.required(Capability::live_edit) &&
             !demand.required(Capability::overlay) &&
             !demand.required(Capability::visual),
         "opening LayerE alone should activate live edit only");
  demand = calculate({}, true, true);
  expect(demand.required(Capability::live_edit) &&
             demand.required(Capability::overlay),
         "edit mode should additionally activate overlay presentation");
}

struct ReentrantContext {
  layerc::runtime::DaemonKernel *kernel{};
  const RuntimeServiceDescriptor *descriptor{};
  bool notified{};
  int quiesced{};
};

ServiceReconcileResult reentrant_reconcile(void *const context) {
  auto &reentrant = *static_cast<ReentrantContext *>(context);
  if (!reentrant.notified) {
    reentrant.notified = true;
    CapabilityDemand cleared;
    (void)reentrant.kernel->reconcile(cleared, {reentrant.descriptor, 1});
  }
  return {};
}

void reentrant_quiesce(void *const context) noexcept {
  ++static_cast<ReentrantContext *>(context)->quiesced;
}

RuntimeResourceStats reentrant_resources(void *) noexcept {
  return {.threads = 1};
}

void kernel_serializes_reentrant_demand() {
  layerc::runtime::DaemonKernel kernel;
  ReentrantContext context{.kernel = &kernel};
  const auto descriptor = RuntimeServiceDescriptor{
      .capability = Capability::visual,
      .context = &context,
      .reconcile = &reentrant_reconcile,
      .quiesce = &reentrant_quiesce,
      .resources = &reentrant_resources};
  context.descriptor = &descriptor;
  CapabilityDemand demand;
  demand.set(Capability::visual, 1);
  expect(kernel.reconcile(demand, {&descriptor, 1}),
         "kernel should serialize a notification raised during reconcile");
  expect(context.notified && context.quiesced == 1 && kernel.demand().empty() &&
             kernel.services().find(Capability::visual) == nullptr &&
             kernel.demand_generation() == 2,
         "latest reentrant demand should win and release the active service");
}

} // namespace

int main() {
  demand_counts_saturate();
  host_is_lazy_and_idempotent();
  faults_release_the_service();
  incomplete_descriptor_faults();
  later_fault_rolls_back_new_dependencies();
  teardown_is_reverse_ordered();
  repeated_activation_has_bounded_ownership();
  demand_matrix_is_explicit();
  kernel_serializes_reentrant_demand();
  if (layerc::test::failures != 0)
    return 1;
  std::cout << "runtime service tests passed\n";
  return 0;
}
