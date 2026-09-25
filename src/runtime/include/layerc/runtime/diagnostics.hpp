#pragma once

#include "layerc/core/json_reader.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/runtime/runtime_services.hpp"

namespace layerc::runtime {

inline constexpr std::uint32_t runtime_diagnostics_schema = 1;
[[nodiscard]] std::string_view runtime_package_version() noexcept;

struct ProcessDiagnostics {
  std::uint32_t pid{};
  std::uint64_t private_bytes{};
  std::uint64_t working_set_bytes{};
  std::uint32_t handles{};
  std::uint32_t threads{};
  std::uint64_t kernel_time_100ns{};
  std::uint64_t user_time_100ns{};
  bool operator==(const ProcessDiagnostics &) const = default;
};
struct TransportDiagnostics {
  bool connected{};
  std::string last_error;
  bool operator==(const TransportDiagnostics &) const = default;
};
struct SettingsDiagnostics {
  bool capture_exclusion{};
  bool visual_recognition{};
  std::string last_error;
  bool operator==(const SettingsDiagnostics &) const = default;
};
struct SessionDiagnostics {
  std::string profile_id;
  std::string source_key;
  std::uint64_t window{};
  bool foreground{};
  std::uint32_t overlay_windows{};
  std::string last_input_match;
  bool operator==(const SessionDiagnostics &) const = default;
};
struct HelperDiagnostics {
  std::uint64_t window{};
  std::uint32_t pid{};
  bool running{};
  bool quarantined{};
  std::uint64_t fault_count{};
  std::string last_error;
  bool operator==(const HelperDiagnostics &) const = default;
};

// The contract belongs beside RuntimeServiceStats; core remains independent
// of the service host and native process/window APIs.
struct RuntimeDiagnostics {
  std::uint32_t schema_version{runtime_diagnostics_schema};
  std::string package_version{runtime_package_version()};
  std::uint32_t protocol_version{core::live_protocol_version};
  ProcessDiagnostics process;
  std::vector<RuntimeServiceStats> services;
  TransportDiagnostics gamebar;
  SettingsDiagnostics settings;
  RuntimeResourceStats aggregate_resources;
  std::vector<SessionDiagnostics> sessions;
  std::vector<HelperDiagnostics> helpers;
  bool operator==(const RuntimeDiagnostics &) const = default;
};

// Linux's native probe/availability extension is typed but has no X11 handles.
// It is emitted alongside the shared runtime diagnostics snapshot.
struct X11ExtensionStatus {
  bool available{};
  int major{};
  int minor{};
};
struct X11StatusSnapshot {
  bool connected{};
  X11ExtensionStatus xi2;
  X11ExtensionStatus shape;
  X11ExtensionStatus randr;
  X11ExtensionStatus composite;
  X11ExtensionStatus damage;
  X11ExtensionStatus xfixes;
  bool ewmh{};
  std::string reason;
};
enum class CapabilityAvailability : std::uint8_t {
  unavailable, degraded, active, inactive,
};
[[nodiscard]] constexpr std::string_view
availability_name(CapabilityAvailability availability) noexcept {
  switch (availability) {
  case CapabilityAvailability::unavailable: return "unavailable";
  case CapabilityAvailability::degraded: return "degraded";
  case CapabilityAvailability::active: return "active";
  case CapabilityAvailability::inactive: return "inactive";
  }
  return "unavailable";
}
struct CapabilityStatus {
  Capability capability{};
  CapabilityAvailability availability{CapabilityAvailability::unavailable};
  std::vector<std::string> reasons;
};
struct LinuxStatusSnapshot {
  X11StatusSnapshot x11;
  std::vector<CapabilityStatus> capabilities;
};

// Appends Linux/X11 probes and availability to the shared diagnostics JSON.
[[nodiscard]] std::string serialize_linux_status_json(
    const RuntimeDiagnostics &diagnostics, const LinuxStatusSnapshot &linux);

using RuntimeDiagnosticsResult =
    std::variant<RuntimeDiagnostics, core::json::FieldError>;
[[nodiscard]] std::string
serialize_runtime_diagnostics_json(const RuntimeDiagnostics &diagnostics);
[[nodiscard]] RuntimeDiagnosticsResult
parse_runtime_diagnostics_json(std::string_view source);

} // namespace layerc::runtime
