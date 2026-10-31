#pragma once

#include "layerc/core/document_error.hpp"
#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::core {
namespace json { class Value; }

inline constexpr std::uint32_t companion_schema_version = 1;
inline constexpr std::size_t max_companion_slots = 8;
inline constexpr std::size_t max_companion_sidecar_bytes = 64 * 1024; // 64 KiB
inline constexpr std::size_t max_companion_snapshot_bytes = 64 * 1024;
inline constexpr std::size_t max_companion_snapshot_panels = 8;
inline constexpr std::size_t max_companion_snapshot_items = 32;
inline constexpr std::size_t max_companion_available_panels = 64;
inline constexpr std::size_t max_companion_text_bytes = 512;
// Snapshot wire versions: an absent snapshotVersion is the cooldown-only
// version 1; version 2 carries resolved typed overlay rows.
inline constexpr std::uint32_t companion_snapshot_version_legacy = 1;
inline constexpr std::uint32_t companion_snapshot_version_typed = 2;

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

using CompanionError = DocumentError;

struct CompanionSnapshotItem {
  std::string cooldown_id;
  std::string name;
  // Fraction of the cooldown elapsed toward ready: 1.0 = ready, 0.0 = just
  // triggered (see build_companion_snapshot).
  double progress{0.0};
  std::uint32_t available_charges{1};
  std::uint32_t max_charges{1};
  std::int64_t remaining_ms{0};
  bool is_ready{true};

  bool operator==(const CompanionSnapshotItem &) const = default;
};
// One version-2 row, fully resolved by the producer from present_panel():
// widgets never resolve references, evaluate variables, run countdowns or
// derive flash phases. Unknown wire kinds reject the whole snapshot.
struct CompanionTypedItem {
  std::string item_id;
  OverlayItemKind kind{OverlayItemKind::cooldown};
  // Resolved label and formatted value, separately bounded to 512 bytes.
  // A whole-row text override uses label alone and leaves value_text empty.
  std::string label;
  std::string value_text;
  // The native bar fill fraction in [0,1]; meaningful when has_progress.
  double fill{1.0};
  bool has_progress{true};
  std::uint32_t pips{};
  std::uint32_t filled_pips{};
  // Resolved 0xRRGGBB fill, pip and indicator accent.
  std::uint32_t color{};
  bool active{};
  // The producer's flash phase; true whenever the row is not flashing.
  bool flash_on{true};
  bool unavailable{};

  bool operator==(const CompanionTypedItem &) const = default;
};

struct CompanionAvailablePanel {
  std::string panel_id;
  std::string name;

  bool operator==(const CompanionAvailablePanel &) const = default;
};

struct CompanionPanelSnapshot {
  std::string panel_id;
  std::uint32_t slot_index{};
  // Version 1 rows; empty in version 2.
  std::vector<CompanionSnapshotItem> items;
  // Version 2 only: resolved rows, resolved panel visibility, and the number
  // of rows left out to honor the item or byte bound.
  std::vector<CompanionTypedItem> typed_items;
  bool visible{true};
  std::uint32_t omitted_items{};

  bool operator==(const CompanionPanelSnapshot &) const = default;
};

struct CompanionSessionSnapshot {
  std::uint32_t snapshot_version{companion_snapshot_version_legacy};
  std::string profile_id;
  // Opaque producer-defined runtime-instance identifier. Consumers must not
  // parse or persist its representation.
  std::string session_key;
  std::uint64_t timestamp_ms{};
  std::vector<CompanionPanelSnapshot> panels;
  // Full bounded panel catalog and current presentation mappings are carried
  // with each immutable snapshot so the settings widget never reads profile
  // files from its app container.
  std::vector<CompanionAvailablePanel> available_panels;
  std::vector<CompanionSlotMapping> preferences;

  bool operator==(const CompanionSessionSnapshot &) const = default;
};

[[nodiscard]] std::variant<CompanionProfile, CompanionError>
parse_companion_profile_json(std::string_view json,
                             const GameProfile &base_profile);
// For a bounded document already accepted by json::parse (including UTF-8).
[[nodiscard]] std::variant<CompanionProfile, CompanionError>
parse_companion_profile_value(const json::Value &root,
                              const GameProfile &base_profile);

// Present means validation failed. Matches optional<CompanionError> so
// `if (auto error = validate_...)` reads as "if there is an error".
using CompanionValidationResult = std::optional<CompanionError>;
using CompanionSerializeResult = std::variant<std::string, CompanionError>;

[[nodiscard]] CompanionValidationResult
validate_companion_profile(const CompanionProfile &profile,
                           const GameProfile &base_profile);

[[nodiscard]] std::string
serialize_companion_profile_json(const CompanionProfile &profile);

[[nodiscard]] CompanionValidationResult
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot);

[[nodiscard]] CompanionSerializeResult
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot);

[[nodiscard]] std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(std::string_view json);

} // namespace layerc::core
