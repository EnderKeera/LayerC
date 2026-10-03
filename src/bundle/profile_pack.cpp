// Profile-pack export: reads the selected stores and writes a byte-stable ZIP.
#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "pack_detail.hpp"
#include "pack_sidecar_table.hpp"

#include "miniz.h"

#include <algorithm>
#include <set>
#include <system_error>
#include <type_traits>
#include <utility>

namespace layerc::bundle {
namespace {

using core::owned_bytes;
using detail::asset_path_for_profile;
using detail::EntryMap;
using detail::kSidecars;
using detail::manifest_json;
using detail::pack_error;
using detail::SidecarPathLists;

template <typename Sidecar>
std::optional<PackError>
sidecar_export_error(const Sidecar &sidecar,
                     const std::filesystem::path &profile_path,
                     const std::string_view kind) {
  if (sidecar.state() == platform::ModuleSidecarState::invalid)
    return PackError{std::string(kind) +
                     " sidecar is invalid: " + profile_path.string()};
  if (sidecar.state() == platform::ModuleSidecarState::recovered_backup)
    return PackError{"save the recovered " + std::string(kind) +
                     " sidecar before exporting: " + profile_path.string()};
  return std::nullopt;
}

template <typename ReadFn, typename Reserve>
std::optional<PackError>
load_sidecar_json_for_export(const std::filesystem::path &sidecar_source,
                             const std::filesystem::path &profile_path,
                             const std::string_view kind, ReadFn &&read_fn,
                             std::string &out_json,
                             const Reserve &reserve_entry) {
  auto json = read_fn(sidecar_source);
  if (!json.has_value())
    return PackError{std::string(kind) +
                     " sidecar could not be read: " + profile_path.string()};
  if (!reserve_entry(json->size()))
    return PackError{detail::pack_limit_message()};
  out_json = std::move(*json);
  return std::nullopt;
}

bool add_zip_entry(mz_zip_archive &zip, const std::string &path,
                   const std::span<const std::byte> bytes) {
  // Fixed entry timestamp keeps pack export byte-stable across runs.
  MZ_TIME_T fixed_time = static_cast<MZ_TIME_T>(315662400);
  return mz_zip_writer_add_mem_ex_v2(&zip, path.c_str(), bytes.data(),
                                     bytes.size(), nullptr, 0,
                                     MZ_BEST_COMPRESSION, 0, 0, &fixed_time,
                                     nullptr, 0, nullptr, 0) != 0;
}
} // namespace

PackOperationResult export_profile_pack(
    const std::filesystem::path &destination,
    const std::span<const std::filesystem::path> profile_paths) {
  if (profile_paths.empty() || profile_paths.size() > max_pack_profiles)
    return pack_error<PackOperationResult>("select between 1 and " +
                   std::to_string(max_pack_profiles) + " profiles to export");

  std::vector<PackProfile> profiles;
  std::map<std::string, PackAsset, std::less<>> assets;
  std::set<std::string, std::less<>> ids;
  std::size_t aggregate_bytes{};
  std::size_t aggregate_entries{1U}; // manifest.json
  const auto reserve_entry = [&](const std::size_t bytes) {
    if (aggregate_entries >= max_pack_entries ||
        bytes > max_pack_bytes - aggregate_bytes)
      return false;
    ++aggregate_entries;
    aggregate_bytes += bytes;
    return true;
  };
  profiles.reserve(profile_paths.size());
  for (const auto &path : profile_paths) {
    const auto base = platform::read_profile_file(path);
    if (!base.has_value())
      return pack_error<PackOperationResult>("profile could not be read: " + path.string());
    const auto parsed = core::parse_profile_json(*base);
    const auto *profile = std::get_if<core::GameProfile>(&parsed);
    if (profile == nullptr)
      return pack_error<PackOperationResult>("profile is invalid: " + path.string());
    if (!ids.insert(profile->id).second)
      return pack_error<PackOperationResult>("selected profiles contain duplicate IDs");
    if (!reserve_entry(base->size()))
      return pack_error<PackOperationResult>(detail::pack_limit_message());

    PackProfile item;
    item.suggested_filename =
        platform::path_to_utf8(path.filename()).value_or("profile.json");
    item.base_json = *base;
    item.profile = *profile;
    for (const auto &desc : kSidecars) {
      const auto sidecar_error = detail::visit_sidecar(
          item, desc,
          [&](auto &payload, const auto &traits) -> std::optional<PackError> {
            using Traits = std::remove_cvref_t<decltype(traits)>;
            using SidecarProfile = typename Traits::Profile;
            const auto sidecar = Traits::load(path, *profile);
            if (auto error = sidecar_export_error(sidecar, path, desc.name))
              return error;
            if (sidecar.state() != platform::ModuleSidecarState::valid ||
                !sidecar.usable())
              return std::nullopt;
            std::string module_json;
            if (auto error = load_sidecar_json_for_export(
                    sidecar.source_path(), path, desc.name, Traits::read,
                    module_json, reserve_entry))
              return error;
            payload = ModulePayload<SidecarProfile>{
                .json = std::move(module_json), .profile = *sidecar.profile()};
            const auto references = Traits::assets(*sidecar.profile());
            if (!references.empty()) item.asset_sha256 = references;
            for (const auto &hash : references) {
              if (assets.contains(hash)) continue;
              auto bytes = platform::read_durable_file(
                  asset_path_for_profile(path, hash), max_pack_asset_bytes);
              if (!bytes.has_value() || core::sha256(*bytes) != hash)
                return PackError{"visual asset is missing or corrupt: " + hash};
              if (!reserve_entry(bytes->size()))
                return PackError{detail::pack_limit_message()};
              assets.try_emplace(
                  hash, PackAsset{.sha256 = hash, .bytes = std::move(*bytes)});
            }
            return std::nullopt;
          });
      if (sidecar_error) return *sidecar_error;
    }
    profiles.push_back(std::move(item));
  }
  std::ranges::sort(profiles, {}, [](const PackProfile &profile) {
    return profile.profile.id;
  });
  std::vector<std::string> base_paths(profiles.size());
  SidecarPathLists sidecar_paths;
  for (auto &paths : sidecar_paths) paths.resize(profiles.size());

  EntryMap entries;
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto ordinal = std::to_string(1000U + index).substr(1);
    base_paths[index] = "profiles/" + ordinal + ".json";
    entries.emplace(base_paths[index], owned_bytes(profiles[index].base_json));
    auto stage_module = [&](const auto &module,
                             std::string &out_path,
                             const std::string_view suffix) {
      if (!module.has_value())
        return;
      out_path = "modules/" + ordinal + std::string(suffix);
      entries.emplace(out_path, owned_bytes(module->json));
    };
    for (const auto &desc : kSidecars) {
      detail::visit_sidecar(
          profiles[index], desc, [&](const auto &module, const auto &) {
            stage_module(module, sidecar_paths[detail::sidecar_index(desc)][index], detail::sidecar_suffix(desc));
          });
    }
  }
  for (const auto &[hash, asset] : assets)
    entries.emplace("assets/" + hash + ".png", asset.bytes);
  const auto manifest =
      manifest_json(profiles, assets, base_paths, sidecar_paths);
  if (manifest.size() > max_pack_bytes - aggregate_bytes)
    return pack_error<PackOperationResult>(
        "pack would exceed " + detail::size_limit_text(max_pack_bytes) +
        " uncompressed");

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 128U * 1024U))
    return pack_error<PackOperationResult>("could not initialize ZIP writer");
  bool ok = add_zip_entry(zip, "manifest.json", owned_bytes(manifest));
  for (const auto &[path, bytes] : entries)
    ok = ok && add_zip_entry(zip, path, bytes);
  void *archive_data{};
  size_t archive_size{};
  ok = ok &&
       mz_zip_writer_finalize_heap_archive(&zip, &archive_data, &archive_size);
  mz_zip_writer_end(&zip);
  if (!ok || archive_data == nullptr || archive_size > max_pack_bytes) {
    if (archive_data != nullptr)
      mz_free(archive_data);
    return pack_error<PackOperationResult>("pack ZIP creation failed or exceeded " +
                                       detail::size_limit_text(max_pack_bytes));
  }
  const auto archive = std::span(
      reinterpret_cast<const std::byte *>(archive_data), archive_size);
  std::error_code directory_error;
  if (!destination.parent_path().empty())
    std::filesystem::create_directories(destination.parent_path(),
                                        directory_error);
  const bool written =
      !directory_error &&
      platform::write_durable_file(destination, archive) ==
          platform::DurablePublishOutcome::published_durable;
  mz_free(archive_data);
  return written ? PackOperationResult{std::monostate{}}
                 : pack_error<PackOperationResult>("pack file could not be written");
}

} // namespace layerc::bundle
