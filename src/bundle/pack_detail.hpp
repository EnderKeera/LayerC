#pragma once

#include "pack_sidecar_table.hpp"

#include "layerc/core/json_reader.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::bundle::detail {

using EntryMap = std::map<std::string, std::vector<std::byte>, std::less<>>;

PackOperationResult failure(std::string message);
PackValidationResult validation_failure(std::string message);

std::string as_string(std::span<const std::byte> bytes);
bool safe_entry_path(std::string_view path);
std::filesystem::path
asset_path_for_profile(const std::filesystem::path &profile_path,
                       std::string_view hash);

std::optional<std::uint32_t> source_schema(std::string_view json);

std::optional<std::string>
manifest_json(const std::vector<PackProfile> &profiles,
              const std::map<std::string, PackAsset, std::less<>> &assets,
              const std::vector<std::string> &base_paths,
              const SidecarPathLists &sidecar_paths);
std::vector<std::string>
visual_asset_references(const core::VisualProfile &profile);
} // namespace layerc::bundle::detail
