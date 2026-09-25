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
#include <utility>
#include <vector>

namespace layerc::bundle::detail {

using EntryMap = std::map<std::string, std::vector<std::byte>, std::less<>>;

template <typename Result>
Result pack_error(std::string message) {
  return PackError{std::move(message)};
}

inline std::string pack_limit_message() {
  return "pack would exceed " + std::to_string(max_pack_entries) +
         " entries or " + std::to_string(max_pack_bytes / (1024U * 1024U)) +
         " MiB uncompressed";
}

inline std::vector<std::byte> owned_bytes(const std::string_view source) {
  const auto bytes = std::as_bytes(std::span(source.data(), source.size()));
  return {bytes.begin(), bytes.end()};
}

// "128 MiB" / "256 KiB": the user-facing spelling of a byte limit.
inline std::string size_limit_text(const std::size_t bytes) {
  constexpr std::size_t mib = 1024U * 1024U;
  return bytes % mib == 0U ? std::to_string(bytes / mib) + " MiB"
                           : std::to_string(bytes / 1024U) + " KiB";
}

std::string as_string(std::span<const std::byte> bytes);
bool safe_entry_path(std::string_view path);
std::filesystem::path
asset_path_for_profile(const std::filesystem::path &profile_path,
                       std::string_view hash);

std::string
manifest_json(const std::vector<PackProfile> &profiles,
              const std::map<std::string, PackAsset, std::less<>> &assets,
              const std::vector<std::string> &base_paths,
              const SidecarPathLists &sidecar_paths);
} // namespace layerc::bundle::detail
