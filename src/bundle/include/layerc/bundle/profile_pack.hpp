#pragma once

#include "layerc/core/profile_modules.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace layerc::bundle {

inline constexpr std::size_t max_pack_bytes = 128U * 1024U * 1024U;
inline constexpr std::size_t max_pack_entries = 256U;
inline constexpr std::size_t max_pack_profiles = 64U;
inline constexpr std::size_t max_pack_asset_bytes = 4U * 1024U * 1024U;

struct PackError {
  std::string message;
};

struct PackAsset {
  std::string sha256;
  std::vector<std::byte> bytes;
};

template <typename Profile> struct ModulePayload {
  std::string json;
  Profile profile;
};

template <typename Module>
using PackModuleSlot = std::optional<ModulePayload<typename Module::Profile>>;

struct PackProfile {
  std::string suggested_filename;
  std::string base_json;
  layerc::core::GameProfile profile;
  // One optional payload per core::ProfileModuleTable entry.
  layerc::core::ProfileModuleTable::each<PackModuleSlot> modules;
  std::vector<std::string> asset_sha256;

  template <typename Module>
  [[nodiscard]] PackModuleSlot<Module> &module() noexcept {
    return std::get<PackModuleSlot<Module>>(modules);
  }
  template <typename Module>
  [[nodiscard]] const PackModuleSlot<Module> &module() const noexcept {
    return std::get<PackModuleSlot<Module>>(modules);
  }
};

struct ValidatedPack {
  std::string producer_version;
  std::vector<PackProfile> profiles;
  std::map<std::string, PackAsset, std::less<>> assets;
};

using PackValidationResult = std::variant<ValidatedPack, PackError>;
using PackOperationResult = std::variant<std::monostate, PackError>;

enum class ImportAction : std::uint8_t {
  skip,
  replace,
  copy,
};

struct ImportChoice {
  std::size_t profile_index{};
  ImportAction action{ImportAction::copy};
  // Optional destination accepted by an interactive preview. A changed plan
  // fails rather than publishing to a destination the user did not review.
  std::optional<std::filesystem::path> expected_target{};
  bool require_absent{};
  // A plain JSON import has no module-removal intent. Full packs retain the
  // existing replacement policy; missing modules in them are removed.
  bool keep_missing_modules{};
};

[[nodiscard]] PackOperationResult export_profile_pack(
    const std::filesystem::path &destination,
    std::span<const std::filesystem::path> profile_paths);

[[nodiscard]] PackValidationResult
validate_profile_pack(const std::filesystem::path &source);

// Resolves the manifest suggestion to the exact top-level JSON basename used
// by import and conflict preview. Nested suggestions never create folders.
[[nodiscard]] std::optional<std::filesystem::path>
resolve_suggested_profile_filename(std::string_view suggested_filename);

[[nodiscard]] PackOperationResult import_profile_pack(
    const ValidatedPack &pack, std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory);

// Advisory, side-effect-free resolved import plan for CLI preview (--dry-run).
// The committing importer rebuilds it under recovered writer ownership using
// the same policy for IDs, filename-aware case-folded destinations, skipped
// entries, and validation errors. Callers keep existing profiles by switching
// colliding bound choices to ImportAction::skip. expected_target binds reviewed
// destinations; the preview does not reserve IDs, targets, or backup contents.
struct ResolvedImportEntry {
  std::size_t profile_index{};
  std::string id;
  std::filesystem::path target;
  bool collides{};
  bool skipped{};
};

struct ImportPreview {
  std::vector<ResolvedImportEntry> entries;
  // Advisory choices bound to reviewed paths, revalidated under ownership.
  std::vector<ImportChoice> choices;
  std::optional<PackError> error;
};

[[nodiscard]] ImportPreview preview_profile_import(
    const ValidatedPack &pack, std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory);

// Side-effect-free validation of the exact choices a commit would use.
// Dry-run consumes this (never the committing importer) so preview/commit
// parity holds and dry-run publishes no profile mutations.
[[nodiscard]] std::optional<PackError> validate_import_choices(
    const ValidatedPack &pack, std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory);

// Resolves journals left by a terminated import. Transactions without a
// durable commit marker are rolled back; committed transactions are cleaned.
[[nodiscard]] PackOperationResult
recover_profile_pack_transactions(const std::filesystem::path &profile_directory);

} // namespace layerc::bundle
