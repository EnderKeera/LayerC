#pragma once

#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/json.hpp"
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

std::optional<std::vector<std::byte>>
read_file(const std::filesystem::path &path, std::size_t maximum);
std::string as_string(std::span<const std::byte> bytes);
std::vector<std::byte> as_bytes(std::string_view source);
bool safe_entry_path(std::string_view path);
std::filesystem::path
asset_path_for_profile_root(const std::filesystem::path &profile_root,
                            std::string_view hash);

const core::json::Value *required(const core::json::Value &object,
                                  std::string_view key);
std::optional<std::string> required_string(const core::json::Value &object,
                                           std::string_view key);
std::optional<std::uint64_t> required_unsigned(const core::json::Value &object,
                                               std::string_view key);
bool exact_fields(const core::json::Value &object,
                  std::initializer_list<std::string_view> allowed);
std::optional<std::uint32_t> source_schema(std::string_view json);

std::optional<std::string>
manifest_json(const std::vector<PackProfile> &profiles,
              const std::map<std::string, PackAsset, std::less<>> &assets,
              const std::vector<std::string> &base_paths,
              const std::vector<std::string> &visual_paths,
              const std::vector<std::string> &automation_paths,
              const std::vector<std::string> &companion_paths);
std::vector<std::string>
visual_asset_references(const core::VisualProfile &profile);
bool write_file_atomically(const std::filesystem::path &destination,
                           std::span<const std::byte> bytes);

} // namespace layerc::bundle::detail
