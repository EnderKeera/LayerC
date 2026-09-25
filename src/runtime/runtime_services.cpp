#include "layerc/runtime/runtime_services.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace layerc::runtime {
namespace {

template <typename Unsigned>
Unsigned saturating_add(const Unsigned left, const Unsigned right) noexcept {
  if (right > std::numeric_limits<Unsigned>::max() - left)
    return std::numeric_limits<Unsigned>::max();
  return left + right;
}

void mark_faulted(RuntimeServiceStats &stats, std::string error) {
  stats.state = ServiceState::faulted;
  stats.resources = {};
  ++stats.fault_count;
  stats.last_error = std::move(error);
}

} // namespace

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
  cache_bytes = saturating_add(cache_bytes, other.cache_bytes);
  return *this;
}

RuntimeServiceHost::RuntimeServiceHost(
    const RuntimeServiceRegistry services) noexcept {
  for (std::size_t slot = 0; slot < services.size(); ++slot) {
    slots_[slot].stats.capability = static_cast<Capability>(slot);
    slots_[slot].service = services[slot];
  }
}

void CapabilityDemand::add_visual_consumer(VisualConsumer consumer) {
  const auto capability = consumer.runtime == core::VisualRuntime::isolated
                              ? Capability::isolated_visual
                              : Capability::visual;
  visual_consumers_.push_back(std::move(consumer));
  add(capability);
}

RuntimeServiceHost::~RuntimeServiceHost() { quiesce_all(); }

std::size_t RuntimeServiceHost::index(const Capability capability) noexcept {
  return static_cast<std::size_t>(capability);
}

bool RuntimeServiceHost::reconcile(
    const CapabilityDemand &demand,
    const core::TimePoint now) {
  bool success = true;
  // Each capability is an independent failure domain: a missing registration
  // or failed service faults only its own slot. Successfully reconciled
  // services stay resident so an unavailable optional capability (for
  // example isolated capture on Linux) can never tear down healthy input.
  // Producers and heavy workers are declared after the state they consume.
  // Retire in reverse order before reconciling active services so capture or
  // other producers cannot deliver into an input/session dependency that has
  // already been torn down.
  for (std::size_t slot_index = slots_.size(); slot_index-- > 0;) {
    const auto capability = static_cast<Capability>(slot_index);
    if (demand.consumers(capability) == 0)
      quiesce_slot(slots_[slot_index], now);
  }
  for (std::size_t slot_index = 0; slot_index < slots_.size(); ++slot_index) {
    auto &slot = slots_[slot_index];
    const auto capability = static_cast<Capability>(slot_index);
    const auto consumers = demand.consumers(capability);
    slot.stats.capability = capability;
    slot.stats.consumers = consumers;

    if (consumers == 0) continue;

    if (slot.service == nullptr) {
      mark_faulted(slot.stats, "required runtime service is unavailable");
      success = false;
      continue;
    }
    if (!slot.active) {
      ++slot.stats.activation_count;
      slot.stats.last_activation = now;
    }

    const auto result = slot.service->reconcile(demand);
    if (!result.success) {
      slot.service->quiesce();
      slot.active = false;
      slot.stats.last_quiescence = now;
      mark_faulted(slot.stats, result.error.empty()
                                   ? "service reconciliation failed"
                                   : result.error);
      success = false;
      continue;
    }

    slot.active = true;
    slot.stats.state = ServiceState::active;
    slot.stats.resources = slot.service->resources();
    slot.stats.last_error = result.error;
  }
  return success;
}

void RuntimeServiceHost::quiesce_slot(Slot &slot,
                                      const core::TimePoint now) noexcept {
  if (slot.active && slot.service != nullptr) {
    slot.service->quiesce();
    slot.active = false;
    slot.stats.last_quiescence = now;
  }
  slot.stats.state = ServiceState::absent;
  slot.stats.consumers = 0;
  slot.stats.resources = {};
  slot.stats.last_error.clear();
}

void RuntimeServiceHost::quiesce_all(const core::TimePoint now) noexcept {
  for (std::size_t slot_index = slots_.size(); slot_index-- > 0;)
    quiesce_slot(slots_[slot_index], now);
}

const IRuntimeService *
RuntimeServiceHost::find(const Capability capability) const noexcept {
  const auto slot = index(capability);
  return slot < slots_.size() && slots_[slot].active
             ? slots_[slot].service
             : nullptr;
}

RuntimeServiceStats
RuntimeServiceHost::stats(const Capability capability) const {
  const auto slot = index(capability);
  if (slot < slots_.size()) {
    auto result = slots_[slot].stats;
    if (slots_[slot].active && slots_[slot].service != nullptr)
      result.resources = slots_[slot].service->resources();
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
  RuntimeResourceStats result = resource_ledger_;
  for (const auto &slot : slots_)
    if (slot.active && slot.service != nullptr)
      result += slot.service->resources();
  return result;
}

} // namespace layerc::runtime
