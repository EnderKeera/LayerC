#include "layerc/runtime/runtime_services.hpp"

#include <algorithm>
#include <limits>

namespace layerc::runtime {
namespace {

std::uint32_t saturating_add(const std::uint32_t left,
                             const std::uint32_t right) noexcept {
  if (right > std::numeric_limits<std::uint32_t>::max() - left)
    return std::numeric_limits<std::uint32_t>::max();
  return left + right;
}

// Generic IRuntimeService built from a descriptor's callbacks. Owns no state
// beyond the descriptor copy, so the wrong-capability class of factory bugs
// is structurally impossible.
class CallbackService final : public IRuntimeService {
public:
  explicit CallbackService(const RuntimeServiceDescriptor &descriptor)
      : capability_(descriptor.capability), context_(descriptor.context),
        reconcile_(descriptor.reconcile), quiesce_(descriptor.quiesce),
        resources_(descriptor.resources) {}

  [[nodiscard]] Capability capability() const noexcept override {
    return capability_;
  }

  [[nodiscard]] ServiceReconcileResult
  reconcile(const CapabilityDemand &) override {
    return reconcile_(context_);
  }

  void quiesce() noexcept override { quiesce_(context_); }

  [[nodiscard]] RuntimeResourceStats
  resources() const noexcept override {
    return resources_(context_);
  }

private:
  Capability capability_;
  void *context_;
  ServiceReconcileFn reconcile_;
  ServiceQuiesceFn quiesce_;
  ServiceResourcesFn resources_;
};

} // namespace

std::string_view capability_name(const Capability capability) noexcept {
  switch (capability) {
  case Capability::input: return "input";
  case Capability::generic_hid: return "genericHid";
  case Capability::overlay: return "overlay";
  case Capability::visual: return "visual";
  case Capability::template_evaluation: return "templateEvaluation";
  case Capability::isolated_visual: return "isolatedVisual";
  case Capability::live_edit: return "liveEdit";
  case Capability::automation: return "automation";
  case Capability::gamebar: return "gamebar";
  case Capability::count: break;
  }
  return "unknown";
}

void CapabilityDemand::add(const Capability capability,
                           const std::uint32_t consumers) noexcept {
  const auto slot = static_cast<std::size_t>(capability);
  if (slot >= consumers_.size()) return;
  consumers_[slot] = saturating_add(consumers_[slot], consumers);
}

void CapabilityDemand::set(const Capability capability,
                           const std::uint32_t consumers) noexcept {
  const auto slot = static_cast<std::size_t>(capability);
  if (slot < consumers_.size()) consumers_[slot] = consumers;
}

std::uint32_t
CapabilityDemand::consumers(const Capability capability) const noexcept {
  const auto slot = static_cast<std::size_t>(capability);
  return slot < consumers_.size() ? consumers_[slot] : 0;
}

bool CapabilityDemand::required(const Capability capability) const noexcept {
  return consumers(capability) != 0;
}

bool CapabilityDemand::empty() const noexcept {
  return std::ranges::all_of(consumers_, [](const auto value) {
    return value == 0;
  });
}

RuntimeResourceStats &
RuntimeResourceStats::operator+=(const RuntimeResourceStats &other) noexcept {
  threads = saturating_add(threads, other.threads);
  timers = saturating_add(timers, other.timers);
  handles = saturating_add(handles, other.handles);
  windows = saturating_add(windows, other.windows);
  objects = saturating_add(objects, other.objects);
  capture_sessions = saturating_add(capture_sessions, other.capture_sessions);
  queues = saturating_add(queues, other.queues);
  decoded_assets = saturating_add(decoded_assets, other.decoded_assets);
  if (other.cache_bytes > std::numeric_limits<std::uint64_t>::max() - cache_bytes)
    cache_bytes = std::numeric_limits<std::uint64_t>::max();
  else
    cache_bytes += other.cache_bytes;
  return *this;
}

RuntimeServiceHost::~RuntimeServiceHost() { quiesce_all(); }

std::size_t RuntimeServiceHost::index(const Capability capability) noexcept {
  return static_cast<std::size_t>(capability);
}

const RuntimeServiceDescriptor *RuntimeServiceHost::descriptor_for(
    const Capability capability,
    const std::span<const RuntimeServiceDescriptor> descriptors) noexcept {
  const auto found = std::ranges::find(descriptors, capability,
                                       &RuntimeServiceDescriptor::capability);
  return found == descriptors.end() ? nullptr : &*found;
}

bool RuntimeServiceHost::reconcile(
    const CapabilityDemand &demand,
    const std::span<const RuntimeServiceDescriptor> descriptors,
    const core::TimePoint now) {
  bool success = true;
  std::vector<std::size_t> activated_this_pass;
  // Producers and heavy workers are declared after the state they consume.
  // Retire in reverse order before reconciling active services so capture or
  // other producers cannot deliver into an input/session dependency that has
  // already been torn down.
  for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot) {
    const auto slot_index = static_cast<std::size_t>(
        std::distance(slot, slots_.rend()) - 1);
    const auto capability = static_cast<Capability>(slot_index);
    if (demand.consumers(capability) == 0) quiesce_slot(*slot, now);
  }
  for (std::size_t slot_index = 0; slot_index < slots_.size(); ++slot_index) {
    auto &slot = slots_[slot_index];
    const auto capability = static_cast<Capability>(slot_index);
    const auto consumers = demand.consumers(capability);
    slot.stats.capability = capability;
    slot.stats.consumers = consumers;

    if (consumers == 0) continue;

    if (!slot.service) {
      const auto *descriptor = descriptor_for(capability, descriptors);
      if (descriptor == nullptr || descriptor->reconcile == nullptr ||
          descriptor->quiesce == nullptr || descriptor->resources == nullptr) {
        slot.stats.state = ServiceState::faulted;
        slot.stats.resources = {};
        ++slot.stats.fault_count;
        slot.stats.last_error = "required service descriptor is incomplete";
        success = false;
        continue;
      }
      slot.service = std::make_unique<CallbackService>(*descriptor);
      ++slot.stats.activation_count;
      slot.stats.last_activation = now;
      activated_this_pass.push_back(slot_index);
    }

    const auto result = slot.service->reconcile(demand);
    if (!result.success) {
      slot.service->quiesce();
      slot.service.reset();
      slot.stats.state = ServiceState::faulted;
      slot.stats.resources = {};
      ++slot.stats.fault_count;
      slot.stats.last_quiescence = now;
      slot.stats.last_error = result.error.empty()
                                  ? "service reconciliation failed"
                                  : result.error;
      success = false;
      continue;
    }

    slot.stats.state = ServiceState::active;
    slot.stats.resources = slot.service->resources();
    slot.stats.last_error.clear();
  }
  if (!success) {
    // A demand transition is atomic for services constructed during this
    // pass. Keep pre-existing services alive, but unwind new dependencies in
    // reverse activation order so no partial capability tier remains resident.
    for (auto activated = activated_this_pass.rbegin();
         activated != activated_this_pass.rend(); ++activated) {
      auto &slot = slots_[*activated];
      if (!slot.service) continue; // the failing service already released itself
      slot.service->quiesce();
      slot.service.reset();
      slot.stats.state = ServiceState::absent;
      slot.stats.consumers = 0;
      slot.stats.resources = {};
      slot.stats.last_quiescence = now;
      slot.stats.last_error.clear();
    }
  }
  return success;
}

void RuntimeServiceHost::quiesce_slot(Slot &slot,
                                      const core::TimePoint now) noexcept {
  if (slot.service) {
    slot.service->quiesce();
    slot.service.reset();
    slot.stats.last_quiescence = now;
  }
  slot.stats.state = ServiceState::absent;
  slot.stats.consumers = 0;
  slot.stats.resources = {};
  slot.stats.last_error.clear();
}

void RuntimeServiceHost::quiesce_all(const core::TimePoint now) noexcept {
  for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot)
    quiesce_slot(*slot, now);
}

IRuntimeService *RuntimeServiceHost::find(const Capability capability) noexcept {
  const auto slot = index(capability);
  return slot < slots_.size() ? slots_[slot].service.get() : nullptr;
}

const IRuntimeService *
RuntimeServiceHost::find(const Capability capability) const noexcept {
  const auto slot = index(capability);
  return slot < slots_.size() ? slots_[slot].service.get() : nullptr;
}

RuntimeServiceStats
RuntimeServiceHost::stats(const Capability capability) const {
  const auto slot = index(capability);
  if (slot < slots_.size()) {
    auto result = slots_[slot].stats;
    if (slots_[slot].service) result.resources = slots_[slot].service->resources();
    return result;
  }
  RuntimeServiceStats result;
  result.capability = capability;
  return result;
}

std::array<RuntimeServiceStats, capability_count>
RuntimeServiceHost::all_stats() const {
  std::array<RuntimeServiceStats, capability_count> result{};
  for (std::size_t index = 0; index < slots_.size(); ++index)
    result[index] = stats(static_cast<Capability>(index));
  return result;
}

RuntimeResourceStats RuntimeServiceHost::aggregate_resources() const noexcept {
  RuntimeResourceStats result;
  for (std::size_t index = 0; index < slots_.size(); ++index)
    result += stats(static_cast<Capability>(index)).resources;
  return result;
}

} // namespace layerc::runtime
