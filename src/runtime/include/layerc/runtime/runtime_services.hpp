#pragma once

#include "layerc/core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace layerc::runtime {

enum class Capability : std::uint8_t {
  input,
  generic_hid,
  overlay,
  visual,
  template_evaluation,
  isolated_visual,
  live_edit,
  automation,
  gamebar,
  count,
};

inline constexpr std::size_t capability_count =
    static_cast<std::size_t>(Capability::count);

[[nodiscard]] std::string_view capability_name(Capability capability) noexcept;

class CapabilityDemand {
public:
  void add(Capability capability, std::uint32_t consumers = 1) noexcept;
  void set(Capability capability, std::uint32_t consumers) noexcept;
  [[nodiscard]] std::uint32_t consumers(Capability capability) const noexcept;
  [[nodiscard]] bool required(Capability capability) const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  bool operator==(const CapabilityDemand &) const = default;

private:
  std::array<std::uint32_t, capability_count> consumers_{};
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

using RuntimeServiceFactory =
    std::unique_ptr<IRuntimeService> (*)(void *context);

struct RuntimeServiceDescriptor {
  Capability capability{Capability::input};
  RuntimeServiceFactory create{};
  void *context{};
};

class RuntimeServiceHost {
public:
  RuntimeServiceHost() = default;
  ~RuntimeServiceHost();
  RuntimeServiceHost(const RuntimeServiceHost &) = delete;
  RuntimeServiceHost &operator=(const RuntimeServiceHost &) = delete;

  [[nodiscard]] bool reconcile(
      const CapabilityDemand &demand,
      std::span<const RuntimeServiceDescriptor> descriptors,
      core::TimePoint now = core::MonotonicClock::now());
  void quiesce_all(core::TimePoint now = core::MonotonicClock::now()) noexcept;

  [[nodiscard]] IRuntimeService *find(Capability capability) noexcept;
  [[nodiscard]] const IRuntimeService *find(Capability capability) const noexcept;
  [[nodiscard]] RuntimeServiceStats stats(Capability capability) const;
  [[nodiscard]] std::array<RuntimeServiceStats, capability_count>
  all_stats() const;
  [[nodiscard]] RuntimeResourceStats aggregate_resources() const noexcept;

private:
  struct Slot {
    std::unique_ptr<IRuntimeService> service;
    RuntimeServiceStats stats;
  };

  [[nodiscard]] static std::size_t index(Capability capability) noexcept;
  [[nodiscard]] static const RuntimeServiceDescriptor *
  descriptor_for(Capability capability,
                 std::span<const RuntimeServiceDescriptor> descriptors) noexcept;
  void quiesce_slot(Slot &slot, core::TimePoint now) noexcept;

  std::array<Slot, capability_count> slots_{};
};

} // namespace layerc::runtime
