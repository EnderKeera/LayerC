#pragma once

#include "layerc/core/enum_wire.hpp"
#include "layerc/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::runtime {

enum class Capability : std::uint8_t {
  input,
  generic_hid,
  overlay,
  visual,
  isolated_visual,
  automation,
  gamebar,
  count,
};

inline constexpr std::size_t capability_count =
    static_cast<std::size_t>(Capability::count);

inline constexpr std::array capability_wire{
    core::EnumWireEntry{Capability::input, "input"},
    core::EnumWireEntry{Capability::generic_hid, "genericHid"},
    core::EnumWireEntry{Capability::overlay, "overlay"},
    core::EnumWireEntry{Capability::visual, "visual"},
    core::EnumWireEntry{Capability::isolated_visual, "isolatedVisual"},
    core::EnumWireEntry{Capability::automation, "automation"},
    core::EnumWireEntry{Capability::gamebar, "gamebar"},
};
static_assert(capability_wire.size() == capability_count);

[[nodiscard]] constexpr std::string_view
capability_name(const Capability capability) noexcept {
  return core::enum_wire_name(capability_wire, capability).value_or("unknown");
}

struct VisualConsumer {
  core::WindowId window;
  std::string profile_id;
  core::VisualRuntime runtime{core::VisualRuntime::inline_process};
  bool operator==(const VisualConsumer &) const = default;
};

class CapabilityDemand {
public:
  void add(Capability capability, std::uint32_t consumers = 1) noexcept;
  void set(Capability capability, std::uint32_t consumers) noexcept;
  [[nodiscard]] std::uint32_t consumers(Capability capability) const noexcept;
  [[nodiscard]] bool required(Capability capability) const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  // Selection and shared-window routing are decided once by the kernel's
  // demand calculation, then consumed unchanged by native capture owners.
  void add_visual_consumer(VisualConsumer consumer);
  [[nodiscard]] std::span<const VisualConsumer>
  visual_consumers() const noexcept { return visual_consumers_; }
  bool operator==(const CapabilityDemand &) const = default;

private:
  std::array<std::uint32_t, capability_count> consumers_{};
  std::vector<VisualConsumer> visual_consumers_;
};

struct RuntimeResourceStats {
  std::uint32_t threads{};
  std::uint32_t timers{};
  std::uint32_t handles{};
  std::uint32_t windows{};
  std::uint32_t objects{};
  std::uint32_t capture_sessions{};
  std::uint32_t queues{};
  std::uint32_t decoded_assets{};
  std::uint64_t cache_bytes{};

  RuntimeResourceStats &operator+=(const RuntimeResourceStats &other) noexcept;
  bool operator==(const RuntimeResourceStats &) const = default;
};

enum class ServiceState : std::uint8_t {
  absent,
  active,
  faulted,
};

// Single owner for the ServiceState wire names shared by the Windows and
// Linux daemon diagnostics.
inline constexpr std::array service_state_wire{
    core::EnumWireEntry{ServiceState::absent, "absent"},
    core::EnumWireEntry{ServiceState::active, "active"},
    core::EnumWireEntry{ServiceState::faulted, "faulted"},
};

[[nodiscard]] constexpr std::string_view
service_state_name(const ServiceState state) noexcept {
  return core::enum_wire_name(service_state_wire, state).value_or("unknown");
}

struct RuntimeServiceStats {
  Capability capability{Capability::input};
  ServiceState state{ServiceState::absent};
  std::uint32_t consumers{};
  RuntimeResourceStats resources;
  std::uint64_t activation_count{};
  std::uint64_t fault_count{};
  std::optional<core::TimePoint> last_activation;
  std::optional<core::TimePoint> last_quiescence;
  std::string last_error;
  bool operator==(const RuntimeServiceStats &) const = default;
};

enum class ServiceReconcileStatus : std::uint8_t {
  ok,       // active; clears the slot's last_error
  degraded, // active; reason is kept as the slot's last_error
  failed,   // quiesced and faulted with reason
};

struct ServiceReconcileResult {
  ServiceReconcileStatus status{ServiceReconcileStatus::ok};
  std::string reason;

  [[nodiscard]] static ServiceReconcileResult degraded(std::string reason) {
    return {ServiceReconcileStatus::degraded, std::move(reason)};
  }
  [[nodiscard]] static ServiceReconcileResult failed(std::string reason) {
    return {ServiceReconcileStatus::failed, std::move(reason)};
  }
};

class IRuntimeService {
public:
  virtual ~IRuntimeService() = default;

  [[nodiscard]] virtual ServiceReconcileResult
  reconcile(const CapabilityDemand &demand) = 0;
  virtual void quiesce() noexcept = 0;
  [[nodiscard]] virtual RuntimeResourceStats resources() const noexcept = 0;
};

// Indexed by Capability: a service serves the capability of its slot.
using RuntimeServiceRegistry =
    std::array<IRuntimeService *, capability_count>;

class RuntimeServiceHost {
public:
  // Registered services and their dependencies must outlive the host.
  explicit RuntimeServiceHost(RuntimeServiceRegistry services = {}) noexcept;
  ~RuntimeServiceHost();
  RuntimeServiceHost(const RuntimeServiceHost &) = delete;
  RuntimeServiceHost &operator=(const RuntimeServiceHost &) = delete;

  [[nodiscard]] bool
  reconcile(const CapabilityDemand &demand,
            core::TimePoint now = core::MonotonicClock::now());
  void quiesce_all(core::TimePoint now = core::MonotonicClock::now()) noexcept;

  // Resources without an independent demand lifecycle (for example an
  // authenticated control-channel handle) belong in this ledger rather than
  // pretending to be a runtime service.
  void set_resource_ledger(RuntimeResourceStats resources) noexcept {
    resource_ledger_ = resources;
  }

  [[nodiscard]] const IRuntimeService *find(Capability capability) const noexcept;
  [[nodiscard]] RuntimeServiceStats stats(Capability capability) const;
  [[nodiscard]] std::array<RuntimeServiceStats, capability_count>
  all_stats() const;
  [[nodiscard]] RuntimeResourceStats aggregate_resources() const noexcept;

private:
  struct Slot {
    IRuntimeService *service{};
    bool active{};
    RuntimeServiceStats stats;
  };

  [[nodiscard]] static std::size_t index(Capability capability) noexcept;
  void quiesce_slot(Slot &slot, core::TimePoint now) noexcept;

  std::array<Slot, capability_count> slots_{};
  RuntimeResourceStats resource_ledger_{};
};

} // namespace layerc::runtime
