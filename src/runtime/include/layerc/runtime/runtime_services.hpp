#pragma once

#include "layerc/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] std::string_view capability_name(Capability capability) noexcept;

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

// Single owner for the ServiceState -> string mapping shared by the Windows
// and Linux daemon diagnostics (previously two identical local copies).
[[nodiscard]] inline const char *
service_state_name(const ServiceState state) noexcept {
  switch (state) {
  case ServiceState::absent: return "absent";
  case ServiceState::active: return "active";
  case ServiceState::faulted: return "faulted";
  }
  return "unknown";
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

struct ServiceReconcileResult {
  bool success{true};
  std::string error;
};

class IRuntimeService {
public:
  virtual ~IRuntimeService() = default;

  [[nodiscard]] virtual Capability capability() const noexcept = 0;
  [[nodiscard]] virtual ServiceReconcileResult
  reconcile(const CapabilityDemand &demand) = 0;
  virtual void quiesce() noexcept = 0;
  [[nodiscard]] virtual RuntimeResourceStats resources() const noexcept = 0;
};

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

  [[nodiscard]] IRuntimeService *find(Capability capability) noexcept;
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
