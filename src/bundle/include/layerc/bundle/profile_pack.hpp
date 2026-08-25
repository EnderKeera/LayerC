#pragma once

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

struct PackProfile {
  std::string suggested_filename;
  std::string base_json;
  layerc::core::GameProfile profile;
  std::optional<std::string> visual_json;
  std::optional<layerc::core::VisualProfile> visual_profile;
  std::optional<std::string> automation_json;
  std::optional<layerc::core::AutomationProfile> automation_profile;
  std::optional<std::string> companion_json;
  std::optional<layerc::core::CompanionProfile> companion_profile;
  std::vector<std::string> asset_sha256;
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

// Resolves journals left by a terminated import. Transactions without a
// durable commit marker are rolled back; committed transactions are cleaned.
[[nodiscard]] PackOperationResult
recover_profile_pack_transactions(const std::filesystem::path &profile_directory);

} // namespace layerc::bundle
