#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

inline constexpr std::uint32_t companion_schema_version = 1;
inline constexpr std::size_t max_companion_slots = 8;
inline constexpr std::size_t max_companion_sidecar_bytes = 64 * 1024; // 64 KiB
inline constexpr std::size_t max_companion_snapshot_bytes = 64 * 1024;
inline constexpr std::size_t max_companion_snapshot_panels = 8;
inline constexpr std::size_t max_companion_snapshot_items = 32;
inline constexpr std::size_t max_companion_text_bytes = 512;

struct CompanionSlotMapping {
  std::uint32_t slot_index{}; // 0..7
  std::string panel_id;
  bool pinned{false};
  bool click_through{true};
  double opacity{1.0};

  bool operator==(const CompanionSlotMapping &) const = default;
};

struct CompanionProfile {
  std::uint32_t schema_version{companion_schema_version};
  std::string profile_id;
  std::vector<CompanionSlotMapping> slots;

  bool operator==(const CompanionProfile &) const = default;
};

struct CompanionError {
  std::string path;
  std::string message;

  bool operator==(const CompanionError &) const = default;
};

struct CompanionSnapshotItem {
  std::string cooldown_id;
  std::string name;
  // Reserved: no per-cooldown icon/color source exists yet; the bridge emits
  // empty strings until a renderer defines the data source.
  std::string icon;
  // Fraction of the cooldown elapsed toward ready: 1.0 = ready, 0.0 = just
  // triggered (see GameBarBridge::create_session_snapshot).
  double progress{0.0};
  std::uint32_t available_charges{1};
  std::uint32_t max_charges{1};
  std::int64_t remaining_ms{0};
  bool is_ready{true};
  std::string color;

  bool operator==(const CompanionSnapshotItem &) const = default;
};

struct CompanionPanelSnapshot {
  std::string panel_id;
  std::uint32_t slot_index{};
  // Reserved: the bridge currently marks every published panel visible until
  // session-level panel visibility is plumbed through.
  bool visible{true};
  std::vector<CompanionSnapshotItem> items;

  bool operator==(const CompanionPanelSnapshot &) const = default;
};

struct CompanionSessionSnapshot {
  std::string profile_id;
  std::string session_key;
  std::uint64_t timestamp_ms{};
  std::vector<CompanionPanelSnapshot> panels;

  bool operator==(const CompanionSessionSnapshot &) const = default;
};

[[nodiscard]] std::variant<CompanionProfile, CompanionError>
parse_companion_profile_json(std::string_view json,
                             const GameProfile &base_profile);

[[nodiscard]] CompanionError
validate_companion_profile(const CompanionProfile &profile,
                           const GameProfile &base_profile);

[[nodiscard]] std::string
serialize_companion_profile_json(const CompanionProfile &profile);

[[nodiscard]] CompanionError
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot);

[[nodiscard]] std::optional<std::string>
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot);

[[nodiscard]] std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(std::string_view json);

} // namespace layerc::core
