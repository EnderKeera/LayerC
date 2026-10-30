#pragma once

#include "layerc/core/json_reader.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/runtime/runtime_services.hpp"
#include "layerc/runtime/display_diagnostics.hpp"

namespace layerc::runtime {

inline constexpr std::uint32_t runtime_diagnostics_schema = 1;

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

struct CaptureDiagnostics {
  std::uint64_t window{};
  core::VisualRuntime runtime{core::VisualRuntime::inline_process};
  core::CaptureHealth health;
  bool operator==(const CaptureDiagnostics &) const = default;
};

// Linux's native probe/availability extension is typed but has no X11 handles.
// Its wire fields belong to the shared runtime diagnostics snapshot.
struct X11ExtensionStatus {
  bool available{};
  int major{};
  int minor{};
  // Native probes omit per-extension reasons; the serializer supplies the
  // connection reason or extension_missing. Parsed wire reasons are retained.
  std::optional<std::string> reason{};
  bool operator==(const X11ExtensionStatus &) const = default;
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
  std::optional<std::string> ewmh_reason{};
  bool operator==(const X11StatusSnapshot &) const = default;
};
enum class CapabilityAvailability : std::uint8_t {
  unavailable, degraded, active, inactive,
};
inline constexpr std::array capability_availability_wire{
    core::EnumWireEntry{CapabilityAvailability::unavailable, "unavailable"},
    core::EnumWireEntry{CapabilityAvailability::degraded, "degraded"},
    core::EnumWireEntry{CapabilityAvailability::active, "active"},
    core::EnumWireEntry{CapabilityAvailability::inactive, "inactive"},
};
[[nodiscard]] constexpr std::string_view
availability_name(CapabilityAvailability availability) noexcept {
  return core::enum_wire_name(capability_availability_wire, availability)
      .value_or("unavailable");
}
struct CapabilityStatus {
  Capability capability{};
  CapabilityAvailability availability{CapabilityAvailability::unavailable};
  std::vector<std::string> reasons;
  // Current session faults in human-readable form; stable reason tokens stay
  // in reasons for clients that classify availability.
  std::vector<std::string> diagnostics;
  bool operator==(const CapabilityStatus &) const = default;
};
struct LinuxDiagnostics {
  X11StatusSnapshot x11;
  std::vector<CapabilityStatus> capabilities;
  // Optional for pre-V2.7 producers; old snapshots round-trip unchanged.
  std::optional<DisplayStatusSnapshot> display{};
  bool operator==(const LinuxDiagnostics &) const = default;
};

// The contract belongs beside RuntimeServiceStats; core remains independent
// of the service host and native process/window APIs.
struct RuntimeDiagnostics {
  std::uint32_t schema_version{runtime_diagnostics_schema};
  std::string package_version{layerc::core::product_version};
  std::uint32_t protocol_version{core::live_protocol_version};
  ProcessDiagnostics process;
  std::vector<RuntimeServiceStats> services;
  TransportDiagnostics gamebar;
  SettingsDiagnostics settings;
  RuntimeResourceStats aggregate_resources;
  std::vector<SessionDiagnostics> sessions;
  std::vector<HelperDiagnostics> helpers;
  std::vector<CaptureDiagnostics> captures;
  // Current automation faults (checkpoint, state load, runtime), one line
  // each, reported identically by both daemons.
  std::vector<std::string> automation_faults;
  // Preserved-but-unsupported overlay items and unavailable item sources,
  // reported identically by both daemons.
  std::vector<std::string> overlay_faults;
  // Absent for producers without Linux probes. Emitted as the existing
  // top-level x11/capabilities fields, not a new nested wire section.
  std::optional<LinuxDiagnostics> linux_extension;
  bool operator==(const RuntimeDiagnostics &) const = default;
};

struct LinuxStatusSnapshot {
  X11StatusSnapshot x11;
  std::vector<CapabilityStatus> capabilities;
  // Shared-schema session rows; emitted as RuntimeDiagnostics::sessions,
  // not inside the x11 extension.
  std::vector<SessionDiagnostics> sessions;
  std::optional<DisplayStatusSnapshot> display{};
  // Shared-schema overlay item diagnostics; warnings only, never capability
  // availability or automation faults.
  std::vector<std::string> overlay_faults;
};

// Adapts native Linux probes to the shared typed diagnostics serializer.
[[nodiscard]] std::string serialize_linux_status_json(
    const RuntimeDiagnostics &diagnostics, const LinuxStatusSnapshot &linux);

using RuntimeDiagnosticsResult =
    std::variant<RuntimeDiagnostics, core::json::FieldError>;
[[nodiscard]] std::string
serialize_runtime_diagnostics_json(const RuntimeDiagnostics &diagnostics);
[[nodiscard]] RuntimeDiagnosticsResult
parse_runtime_diagnostics_json(std::string_view source);

} // namespace layerc::runtime
