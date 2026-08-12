#include "layerc/runtime/daemon_kernel.hpp"
#include "test_support.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using layerc::test::expect;

struct FakeContext {
  layerc::runtime::Capability capability{layerc::runtime::Capability::visual};
  int constructed{};
  int reconciled{};
  int quiesced{};
  bool fail{};
  std::vector<layerc::runtime::Capability> *quiesce_order{};
};

class FakeService final : public layerc::runtime::IRuntimeService {
public:
  explicit FakeService(FakeContext &context) : context_(context) {
    ++context_.constructed;
  }

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return context_.capability;
  }

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override {
    ++context_.reconciled;
    consumers_ = demand.consumers(context_.capability);
    return context_.fail
               ? layerc::runtime::ServiceReconcileResult{false, "test fault"}
               : layerc::runtime::ServiceReconcileResult{};
  }

  void quiesce() noexcept override {
    ++context_.quiesced;
    if (context_.quiesce_order != nullptr)
      context_.quiesce_order->push_back(context_.capability);
  }

  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override {
    return {.threads = 1, .handles = consumers_, .capture_sessions = 1};
  }

private:
  FakeContext &context_;
  std::uint32_t consumers_{};
};

std::unique_ptr<layerc::runtime::IRuntimeService>
make_fake(void *context) {
  return std::make_unique<FakeService>(*static_cast<FakeContext *>(context));
}

void demand_counts_saturate() {
  layerc::runtime::CapabilityDemand demand;
  demand.add(layerc::runtime::Capability::input, 2);
  demand.add(layerc::runtime::Capability::input, 3);
  expect(demand.consumers(layerc::runtime::Capability::input) == 5,
         "capability consumers should accumulate");
  expect(demand.required(layerc::runtime::Capability::input),
         "non-zero demand should be required");
  expect(!demand.empty(), "non-zero demand should not be empty");
  demand.set(layerc::runtime::Capability::input, 0);
  expect(demand.empty(), "cleared demand should be empty");
}

void host_is_lazy_and_idempotent() {
  FakeContext context;
  const layerc::runtime::RuntimeServiceDescriptor descriptor{
      .capability = layerc::runtime::Capability::visual,
      .create = &make_fake,
      .context = &context};
  layerc::runtime::RuntimeServiceHost host;
  layerc::runtime::CapabilityDemand demand;

  expect(host.reconcile(demand, {&descriptor, 1}),
         "empty demand should reconcile");
  expect(context.constructed == 0,
         "empty demand must not construct a service");

  demand.set(layerc::runtime::Capability::visual, 2);
  expect(host.reconcile(demand, {&descriptor, 1}),
         "active demand should reconcile");
  expect(context.constructed == 1 && context.reconciled == 1,
         "first demand should construct and reconcile once");
  expect(host.reconcile(demand, {&descriptor, 1}),
         "repeated demand should reconcile");
  expect(context.constructed == 1 && context.reconciled == 2,
         "repeated demand must reuse the service");

  const auto stats = host.stats(layerc::runtime::Capability::visual);
  expect(stats.state == layerc::runtime::ServiceState::active &&
             stats.consumers == 2 && stats.resources.threads == 1 &&
             stats.resources.handles == 2,
         "active statistics should reflect service resources");

  demand.set(layerc::runtime::Capability::visual, 0);
  expect(host.reconcile(demand, {&descriptor, 1}),
         "zero demand should quiesce successfully");
  expect(context.quiesced == 1 &&
             host.find(layerc::runtime::Capability::visual) == nullptr,
         "zero demand must quiesce and destroy the service");
  expect(host.stats(layerc::runtime::Capability::visual).state ==
             layerc::runtime::ServiceState::absent,
         "quiesced service should report absent");
}

void faults_release_the_service() {
  FakeContext context;
  context.fail = true;
  const layerc::runtime::RuntimeServiceDescriptor descriptor{
      .capability = layerc::runtime::Capability::visual,
      .create = &make_fake,
      .context = &context};
  layerc::runtime::RuntimeServiceHost host;
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::visual, 1);

  expect(!host.reconcile(demand, {&descriptor, 1}),
         "service failure should fail reconciliation");
  const auto stats = host.stats(layerc::runtime::Capability::visual);
  expect(context.quiesced == 1 &&
             host.find(layerc::runtime::Capability::visual) == nullptr,
         "failed service should be quiesced and destroyed");
  expect(stats.state == layerc::runtime::ServiceState::faulted &&
             stats.fault_count == 1 && stats.last_error == "test fault",
         "failure diagnostics should be retained");
}

void later_fault_rolls_back_new_dependencies() {
  FakeContext input;
  input.capability = layerc::runtime::Capability::input;
  FakeContext visual;
  visual.capability = layerc::runtime::Capability::visual;
  visual.fail = true;
  const layerc::runtime::RuntimeServiceDescriptor descriptors[] = {
      {.capability = layerc::runtime::Capability::input,
       .create = &make_fake,
       .context = &input},
      {.capability = layerc::runtime::Capability::visual,
       .create = &make_fake,
       .context = &visual}};
  layerc::runtime::RuntimeServiceHost host;
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::input, 1);
  demand.set(layerc::runtime::Capability::visual, 1);

  expect(!host.reconcile(demand, descriptors),
         "a later service fault should fail the capability transition");
  expect(input.quiesced == 1 && visual.quiesced == 1 &&
             host.find(layerc::runtime::Capability::input) == nullptr &&
             host.find(layerc::runtime::Capability::visual) == nullptr,
         "newly activated dependencies should roll back with the failed tier");
  expect(host.stats(layerc::runtime::Capability::visual).state ==
                 layerc::runtime::ServiceState::faulted &&
             host.stats(layerc::runtime::Capability::input).state ==
                 layerc::runtime::ServiceState::absent,
         "the root fault should remain diagnosable after dependency rollback");

  visual.fail = false;
  expect(host.reconcile(demand, descriptors) && input.constructed == 2 &&
             visual.constructed == 2,
         "a corrected demand should retry every rolled-back capability");
}

void teardown_is_reverse_ordered() {
  std::vector<layerc::runtime::Capability> order;
  FakeContext input;
  input.capability = layerc::runtime::Capability::input;
  input.quiesce_order = &order;
  FakeContext visual;
  visual.capability = layerc::runtime::Capability::visual;
  visual.quiesce_order = &order;
  const layerc::runtime::RuntimeServiceDescriptor descriptors[] = {
      {.capability = layerc::runtime::Capability::input,
       .create = &make_fake,
       .context = &input},
      {.capability = layerc::runtime::Capability::visual,
       .create = &make_fake,
       .context = &visual}};
  layerc::runtime::RuntimeServiceHost host;
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::input, 1);
  demand.set(layerc::runtime::Capability::visual, 1);
  expect(host.reconcile(demand, descriptors),
         "multiple services should activate");
  demand = {};
  expect(host.reconcile(demand, descriptors),
         "multiple services should quiesce");
  expect(order.size() == 2 &&
             order[0] == layerc::runtime::Capability::visual &&
             order[1] == layerc::runtime::Capability::input,
         "services should quiesce in reverse dependency order");
}

void repeated_activation_has_bounded_ownership() {
  FakeContext context;
  const layerc::runtime::RuntimeServiceDescriptor descriptor{
      .capability = layerc::runtime::Capability::visual,
      .create = &make_fake,
      .context = &context};
  layerc::runtime::RuntimeServiceHost host;
  layerc::runtime::CapabilityDemand demand;
  for (int cycle = 0; cycle < 100; ++cycle) {
    demand.set(layerc::runtime::Capability::visual, 1);
    expect(host.reconcile(demand, {&descriptor, 1}),
           "activation cycle should succeed");
    demand.set(layerc::runtime::Capability::visual, 0);
    expect(host.reconcile(demand, {&descriptor, 1}),
           "quiescence cycle should succeed");
  }
  const auto stats = host.stats(layerc::runtime::Capability::visual);
  expect(context.constructed == 100 && context.quiesced == 100,
         "every activation should have one matching quiescence");
  expect(stats.activation_count == 100 &&
             stats.state == layerc::runtime::ServiceState::absent &&
             stats.resources == layerc::runtime::RuntimeResourceStats{},
         "inactive lifecycle statistics should retain counts but no resources");
}

void demand_matrix_is_explicit() {
  using layerc::runtime::Capability;
  using layerc::runtime::SessionCapabilityRequest;
  const auto calculate = [](std::initializer_list<SessionCapabilityRequest> s,
                            const bool editor = false,
                            const bool edit = false) {
    return layerc::runtime::calculate_capability_demand(
        {s.begin(), s.size()}, editor, edit);
  };

  expect(calculate({}).empty(), "no game should demand no service");
  expect(calculate({{.window = {1}, .active = false}}).empty(),
         "a background non-global game should demand no service");

  auto demand = calculate({{.window = {1}, .active = true}});
  expect(demand.consumers(Capability::input) == 1 &&
             demand.consumers(Capability::overlay) == 1 &&
             !demand.required(Capability::visual),
         "input-only foreground game should own input and overlay only");

  demand = calculate({{.window = {1}, .active = true, .global = true,
                       .visual = true, .templates = true}});
  expect(demand.required(Capability::input) &&
             !demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "global profiles should not activate capture");

  demand = calculate({{.window = {1}, .active = true, .visual = true}});
  expect(demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "pixel observations should activate inline visual only");

  demand = calculate({{.window = {1}, .active = true, .visual = true,
                       .templates = true, .generic_hid = true}});
  expect(demand.required(Capability::visual) &&
             demand.required(Capability::template_evaluation) &&
             demand.required(Capability::generic_hid),
         "template and HID consumers should activate their lazy children");

  demand = calculate({
      {.window = {9}, .active = true, .visual = true, .templates = true},
      {.window = {9}, .active = true, .visual = true,
       .isolated_visual = true},
  });
  expect(demand.consumers(Capability::isolated_visual) == 2 &&
             !demand.required(Capability::visual) &&
             !demand.required(Capability::template_evaluation),
         "one isolated profile should route every profile on its HWND through one isolated tier");

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
  const layerc::runtime::RuntimeServiceDescriptor *descriptor{};
  bool notified{};
  int quiesced{};
};

class ReentrantService final : public layerc::runtime::IRuntimeService {
public:
  explicit ReentrantService(ReentrantContext &context) : context_(context) {}
  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::visual;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &) override {
    if (!context_.notified) {
      context_.notified = true;
      layerc::runtime::CapabilityDemand cleared;
      (void)context_.kernel->reconcile(cleared,
                                      {context_.descriptor, 1});
    }
    return {};
  }
  void quiesce() noexcept override { ++context_.quiesced; }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override { return {.threads = 1}; }

private:
  ReentrantContext &context_;
};

std::unique_ptr<layerc::runtime::IRuntimeService>
make_reentrant(void *context) {
  return std::make_unique<ReentrantService>(
      *static_cast<ReentrantContext *>(context));
}

void kernel_serializes_reentrant_demand() {
  layerc::runtime::DaemonKernel kernel;
  ReentrantContext context{.kernel = &kernel};
  const layerc::runtime::RuntimeServiceDescriptor descriptor{
      .capability = layerc::runtime::Capability::visual,
      .create = &make_reentrant,
      .context = &context};
  context.descriptor = &descriptor;
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::visual, 1);
  expect(kernel.reconcile(demand, {&descriptor, 1}),
         "kernel should serialize a notification raised during reconcile");
  expect(context.notified && context.quiesced == 1 && kernel.demand().empty() &&
             kernel.services().find(layerc::runtime::Capability::visual) ==
                 nullptr &&
             kernel.demand_generation() == 2,
         "latest reentrant demand should win and release the active service");
}

} // namespace

int main() {
  demand_counts_saturate();
  host_is_lazy_and_idempotent();
  faults_release_the_service();
  later_fault_rolls_back_new_dependencies();
  teardown_is_reverse_ordered();
  repeated_activation_has_bounded_ownership();
  demand_matrix_is_explicit();
  kernel_serializes_reentrant_demand();
  if (layerc::test::failures != 0) return 1;
  std::cout << "runtime service tests passed\n";
  return 0;
}
