#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "pack_detail.hpp"
#include "pack_sidecar_table.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

#include <windows.h>

namespace layerc::bundle {
namespace {

using detail::as_bytes;
using detail::as_string;
using detail::asset_path_for_profile_root;
using detail::EntryMap;
using detail::exact_fields;
using detail::failure;
using detail::manifest_json;
using detail::read_file;
using detail::required;
using detail::required_string;
using detail::required_unsigned;
using detail::safe_entry_path;
using detail::source_schema;
using detail::validation_failure;
using detail::visual_asset_references;
using detail::write_file_atomically;

template <typename Sidecar>
std::optional<PackError>
sidecar_export_error(const Sidecar &sidecar,
                     const std::filesystem::path &profile_path,
                     const std::string_view kind) {
  if (sidecar.state == decltype(sidecar.state)::invalid)
    return PackError{std::string(kind) +
                     " sidecar is invalid: " + profile_path.string()};
  if (sidecar.state == decltype(sidecar.state)::recovered_backup)
    return PackError{"save the recovered " + std::string(kind) +
                     " sidecar before exporting: " + profile_path.string()};
  return std::nullopt;
}

template <typename ReadFn, typename Reserve>
std::optional<PackError>
load_sidecar_json_for_export(const std::filesystem::path &sidecar_source,
                             const std::filesystem::path &profile_path,
                             const std::string_view kind, ReadFn &&read_fn,
                             std::optional<std::string> &out_json,
                             const Reserve &reserve_entry) {
  auto json = read_fn(sidecar_source);
  if (!json.has_value())
    return PackError{std::string(kind) +
                     " sidecar could not be read: " + profile_path.string()};
  if (!reserve_entry(json->size()))
    return PackError{"pack would exceed 256 entries or 128 MiB uncompressed"};
  out_json = std::move(json);
  return std::nullopt;
}

bool add_zip_entry(mz_zip_archive &zip, const std::string &path,
                   const std::span<const std::byte> bytes) {
  MZ_TIME_T fixed_time = static_cast<MZ_TIME_T>(315662400);
  return mz_zip_writer_add_mem_ex_v2(&zip, path.c_str(), bytes.data(),
                                     bytes.size(), nullptr, 0,
                                     MZ_BEST_COMPRESSION, 0, 0, &fixed_time,
                                     nullptr, 0, nullptr, 0) != 0;
}

using detail::kSidecars;
using detail::sidecar_desc_for_kind;
using detail::SidecarKind;

} // namespace

std::optional<std::filesystem::path>
resolve_suggested_profile_filename(const std::string_view suggested_filename) {
  auto filename = platform::try_utf8_to_wide(suggested_filename);
  if (!filename.has_value() || filename->empty())
    return std::nullopt;
  for (auto &character : *filename) {
    if (std::wstring_view(L"<>\"|?*").find(character) !=
        std::wstring_view::npos)
      character = L'_';
  }
  while (!filename->empty() &&
         (filename->back() == L'.' || filename->back() == L' '))
    filename->pop_back();
  std::filesystem::path resolved(*filename);
  if (_wcsicmp(resolved.extension().c_str(), L".json") != 0)
    return std::nullopt;
  resolved = resolved.filename();
  return resolved.empty() ? std::nullopt
                          : std::optional<std::filesystem::path>(resolved);
}

PackOperationResult export_profile_pack(
    const std::filesystem::path &destination,
    const std::span<const std::filesystem::path> profile_paths) {
  if (profile_paths.empty() || profile_paths.size() > max_pack_profiles)
    return failure("select between 1 and 64 profiles to export");

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
      return failure("profile could not be read: " + path.string());
    const auto parsed = core::parse_profile_json(*base);
    const auto *profile = std::get_if<core::GameProfile>(&parsed);
    if (profile == nullptr)
      return failure("profile is invalid: " + path.string());
    if (!ids.insert(profile->id).second)
      return failure("selected profiles contain duplicate IDs");
    if (!reserve_entry(base->size()))
      return failure("pack would exceed 256 entries or 128 MiB uncompressed");

    PackProfile item;
    item.suggested_filename =
        platform::try_wide_to_utf8(path.filename().wstring())
            .value_or("profile.json");
    item.base_json = *base;
    item.profile = *profile;
    for (const auto &desc : kSidecars) {
      switch (desc.type) {
      case SidecarKind::visual: {
        const auto sidecar = platform::load_visual_sidecar(path, *profile);
        if (auto err = sidecar_export_error(sidecar, path, desc.kind))
          return *err;
        if (sidecar.state == platform::VisualSidecarState::valid &&
            sidecar.profile.has_value()) {
          item.visual_profile = sidecar.profile;
          if (auto error = load_sidecar_json_for_export(
                  sidecar.source_path, path, desc.kind,
                  platform::read_visual_file, item.visual_json, reserve_entry))
            return *error;
          item.asset_sha256 = visual_asset_references(*sidecar.profile);
          for (const auto &hash : item.asset_sha256) {
            if (assets.contains(hash))
              continue;
            const auto bytes =
                read_file(asset_path_for_profile_root(path.parent_path(), hash),
                          max_pack_asset_bytes);
            if (!bytes.has_value() || core::sha256(*bytes) != hash)
              return failure("visual asset is missing or corrupt: " + hash);
            if (!reserve_entry(bytes->size()))
              return failure(
                  "pack would exceed 256 entries or 128 MiB uncompressed");
            assets.try_emplace(
                hash, PackAsset{.sha256 = hash, .bytes = std::move(*bytes)});
          }
        }
        break;
      }
      case SidecarKind::automation: {
        const auto sidecar = platform::load_automation_sidecar(path, *profile);
        if (auto err = sidecar_export_error(sidecar, path, desc.kind))
          return *err;
        if (sidecar.state == platform::AutomationSidecarState::valid &&
            sidecar.profile.has_value()) {
          item.automation_profile = sidecar.profile;
          if (auto error = load_sidecar_json_for_export(
                  sidecar.source_path, path, desc.kind,
                  platform::read_automation_file, item.automation_json,
                  reserve_entry))
            return *error;
        }
        break;
      }
      case SidecarKind::companion: {
        const auto sidecar = platform::load_companion_sidecar(path, *profile);
        if (auto err = sidecar_export_error(sidecar, path, desc.kind))
          return *err;
        if (sidecar.state == platform::CompanionSidecarState::valid &&
            sidecar.profile.has_value()) {
          item.companion_profile = sidecar.profile;
          if (auto error = load_sidecar_json_for_export(
                  sidecar.source_path, path, desc.kind,
                  platform::read_companion_file, item.companion_json,
                  reserve_entry))
            return *error;
        }
        break;
      }
      }
    }
    profiles.push_back(std::move(item));
  }
  std::ranges::sort(profiles, {}, [](const PackProfile &profile) {
    return profile.profile.id;
  });
  std::vector<std::string> base_paths(profiles.size());
  std::vector<std::string> visual_paths(profiles.size());
  std::vector<std::string> automation_paths(profiles.size());
  std::vector<std::string> companion_paths(profiles.size());

  EntryMap entries;
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    const auto ordinal = std::to_string(1000U + index).substr(1);
    base_paths[index] = "profiles/" + ordinal + ".json";
    entries.emplace(base_paths[index], as_bytes(profiles[index].base_json));
    auto stage_module = [&](const std::optional<std::string> &json,
                            std::string &out_path,
                            const std::string_view suffix) {
      if (!json.has_value())
        return;
      out_path = "modules/" + ordinal + std::string(suffix);
      entries.emplace(out_path, as_bytes(*json));
    };
    for (const auto &desc : kSidecars) {
      switch (desc.type) {
      case SidecarKind::visual:
        stage_module(profiles[index].visual_json, visual_paths[index],
                     desc.suffix);
        break;
      case SidecarKind::automation:
        stage_module(profiles[index].automation_json, automation_paths[index],
                     desc.suffix);
        break;
      case SidecarKind::companion:
        stage_module(profiles[index].companion_json, companion_paths[index],
                     desc.suffix);
        break;
      }
    }
  }
  for (const auto &[hash, asset] : assets)
    entries.emplace("assets/" + hash + ".png", asset.bytes);
  const auto manifest =
      manifest_json(profiles, assets, base_paths, visual_paths,
                    automation_paths, companion_paths);
  if (!manifest.has_value())
    return failure("could not hash profile data for the pack manifest");
  if (manifest->size() > max_pack_bytes - aggregate_bytes)
    return failure("pack would exceed 128 MiB uncompressed");

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 128U * 1024U))
    return failure("could not initialize ZIP writer");
  bool ok = add_zip_entry(zip, "manifest.json", as_bytes(*manifest));
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
    return failure("pack ZIP creation failed or exceeded 128 MiB");
  }
  const auto archive = std::span(
      reinterpret_cast<const std::byte *>(archive_data), archive_size);
  const bool written = write_file_atomically(destination, archive);
  mz_free(archive_data);
  return written ? PackOperationResult{std::monostate{}}
                 : failure("pack file could not be written");
}

PackValidationResult
validate_profile_pack(const std::filesystem::path &source) {
  const auto archive_bytes = read_file(source, max_pack_bytes);
  if (!archive_bytes.has_value())
    return validation_failure("pack could not be read or exceeds 128 MiB");

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, archive_bytes->data(),
                              archive_bytes->size(), 0))
    return validation_failure("file is not a valid ZIP archive");
  const auto close_zip = [](mz_zip_archive *archive) {
    (void)mz_zip_reader_end(archive);
  };
  std::unique_ptr<mz_zip_archive, decltype(close_zip)> zip_guard{
      &zip, close_zip};
  if (mz_zip_is_zip64(&zip))
    return validation_failure("ZIP64 packs are not supported");
  const auto count = mz_zip_reader_get_num_files(&zip);
  if (count == 0 || count > max_pack_entries)
    return validation_failure("pack must contain between 1 and 256 entries");

  EntryMap entries;
  std::set<std::wstring, std::less<>> folded_paths;
  std::uint64_t total_uncompressed{};
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, index, &stat))
      return validation_failure("pack central directory is invalid");
    if (stat.m_is_directory)
      return validation_failure("directory entries are not allowed");
    if (stat.m_is_encrypted || !stat.m_is_supported)
      return validation_failure(
          "encrypted or unsupported ZIP entries are not allowed");
    const auto unix_mode = (stat.m_external_attr >> 16U) & 0170000U;
    if (unix_mode == 0120000U ||
        (stat.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
      return validation_failure("symlink or reparse entries are not allowed");
    const auto filename_size =
        mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (filename_size <= 1U || filename_size > 4096U)
      return validation_failure("pack entry name is empty or too long");
    std::string path(static_cast<std::size_t>(filename_size) - 1U, '\0');
    if (mz_zip_reader_get_filename(&zip, index, path.data(), filename_size) !=
        filename_size)
      return validation_failure("pack entry name could not be read");
    if (!safe_entry_path(path))
      return validation_failure("unsafe pack entry path: " + path);
    const auto folded = platform::try_casefold_path(path);
    if (!folded.has_value() || !folded_paths.insert(*folded).second)
      return validation_failure("duplicate or case-colliding pack entry: " +
                                path);
    if (stat.m_uncomp_size > max_pack_bytes ||
        total_uncompressed > max_pack_bytes - stat.m_uncomp_size)
      return validation_failure("pack expands beyond 128 MiB");
    total_uncompressed += stat.m_uncomp_size;
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(),
                                      0))
      return validation_failure("pack entry failed CRC or decompression: " +
                                path);
    entries.emplace(std::move(path), std::move(bytes));
  }
  if (!mz_zip_validate_archive(&zip, 0))
    return validation_failure("ZIP archive validation failed");
  zip_guard.reset();

  const auto manifest_entry = entries.find("manifest.json");
  if (manifest_entry == entries.end() ||
      manifest_entry->second.size() > core::max_profile_json_bytes)
    return validation_failure("manifest.json is missing or exceeds 256 KiB");
  const auto manifest_source = as_string(manifest_entry->second);
  const auto parsed_manifest = core::json::parse(manifest_source);
  const auto *manifest = std::get_if<core::json::Value>(&parsed_manifest);
  if (manifest == nullptr || !manifest->is_object() ||
      !exact_fields(*manifest, {"schemaVersion", "kind", "producerVersion",
                                "profiles", "assets"}))
    return validation_failure("manifest root is invalid or has unknown fields");
  const auto manifest_schema = required_unsigned(*manifest, "schemaVersion");
  const auto kind = required_string(*manifest, "kind");
  const auto producer = required_string(*manifest, "producerVersion");
  const auto *profile_values = required(*manifest, "profiles");
  const auto *asset_values = required(*manifest, "assets");
  if (manifest_schema != 1U || kind != "layerc-profile-pack" ||
      !producer.has_value() || profile_values == nullptr ||
      !profile_values->is_array() || asset_values == nullptr ||
      !asset_values->is_array())
    return validation_failure(
        "manifest identity or collection fields are invalid");
  if (profile_values->as_array().empty() ||
      profile_values->as_array().size() > max_pack_profiles)
    return validation_failure(
        "manifest must declare between 1 and 64 profiles");

  ValidatedPack pack;
  pack.producer_version = *producer;
  std::set<std::string, std::less<>> declared_paths{"manifest.json"};
  for (const auto &value : asset_values->as_array()) {
    if (!exact_fields(value, {"sha256", "path", "size"}))
      return validation_failure(
          "asset descriptor has unknown or missing fields");
    const auto hash = required_string(value, "sha256");
    const auto path = required_string(value, "path");
    const auto size = required_unsigned(value, "size");
    if (!hash.has_value() || !core::is_lowercase_sha256(*hash) ||
        !path.has_value() || !size.has_value() ||
        *size > max_pack_asset_bytes || *path != "assets/" + *hash + ".png")
      return validation_failure("asset descriptor is invalid");
    const auto found = entries.find(*path);
    if (found == entries.end() || found->second.size() != *size ||
        core::sha256(found->second) != *hash)
      return validation_failure("asset size or SHA-256 mismatch: " + *hash);
    if (!declared_paths.insert(*path).second ||
        !pack.assets
             .emplace(*hash, PackAsset{.sha256 = *hash, .bytes = found->second})
             .second)
      return validation_failure("duplicate asset declaration: " + *hash);
  }

  std::set<std::string, std::less<>> profile_ids;
  for (const auto &value : profile_values->as_array()) {
    if (!exact_fields(value,
                      {"id", "suggestedFilename", "base", "modules", "assets"}))
      return validation_failure(
          "profile descriptor has unknown or missing fields");
    const auto id = required_string(value, "id");
    const auto suggested = required_string(value, "suggestedFilename");
    const auto *base = required(value, "base");
    const auto *modules = required(value, "modules");
    const auto *references = required(value, "assets");
    if (!id.has_value() || id->empty() || !profile_ids.insert(*id).second ||
        !suggested.has_value() || suggested->empty() ||
        !safe_entry_path(*suggested) || base == nullptr || modules == nullptr ||
        !modules->is_array() || references == nullptr ||
        !references->is_array())
      return validation_failure("profile descriptor identity is invalid");
    if (!exact_fields(*base, {"path", "schemaVersion", "size", "sha256"}))
      return validation_failure("base profile descriptor is invalid");
    const auto base_path = required_string(*base, "path");
    const auto base_schema = required_unsigned(*base, "schemaVersion");
    const auto base_size = required_unsigned(*base, "size");
    const auto base_hash = required_string(*base, "sha256");
    if (!base_path.has_value() || !base_schema.has_value() ||
        *base_schema < 1U || *base_schema > 4U || !base_size.has_value() ||
        *base_size > core::max_profile_json_bytes || !base_hash.has_value() ||
        !core::is_lowercase_sha256(*base_hash))
      return validation_failure("base profile descriptor fields are invalid");
    const auto base_entry = entries.find(*base_path);
    if (base_entry == entries.end() ||
        base_entry->second.size() != *base_size ||
        core::sha256(base_entry->second) != *base_hash ||
        !declared_paths.insert(*base_path).second)
      return validation_failure("base profile size, hash, or path mismatch");
    const auto base_json = as_string(base_entry->second);
    if (source_schema(base_json) != *base_schema)
      return validation_failure("base profile schema does not match manifest");
    const auto parsed_profile = core::parse_profile_json(base_json);
    const auto *profile = std::get_if<core::GameProfile>(&parsed_profile);
    if (profile == nullptr || profile->id != *id)
      return validation_failure(
          "base profile is invalid or has a mismatched ID");

    PackProfile item;
    item.suggested_filename = *suggested;
    item.base_json = base_json;
    item.profile = *profile;
    std::map<std::string, std::uint32_t, std::less<>> packed_schemas;
    if (modules->as_array().size() > kSidecars.size())
      return validation_failure(
          "at most one visual, automation, and companion module are supported");
    std::set<std::string, std::less<>> module_kinds;
    for (const auto &module : modules->as_array()) {
      if (!exact_fields(module,
                        {"kind", "path", "schemaVersion", "size", "sha256"}))
        return validation_failure(
            "module descriptor has unknown or missing fields");
      const auto module_kind = required_string(module, "kind");
      const auto module_path = required_string(module, "path");
      const auto module_schema = required_unsigned(module, "schemaVersion");
      const auto module_size = required_unsigned(module, "size");
      const auto module_hash = required_string(module, "sha256");
      if (!module_kind.has_value() ||
          !module_kinds.insert(*module_kind).second ||
          !module_path.has_value() || !module_schema.has_value() ||
          *module_schema < 1U || !module_size.has_value() ||
          !module_hash.has_value() || !core::is_lowercase_sha256(*module_hash))
        return validation_failure("unsupported or invalid module descriptor");
      const auto desc = sidecar_desc_for_kind(*module_kind);
      if (!desc.has_value() || *module_schema > desc->max_schema ||
          *module_size > desc->max_bytes)
        return validation_failure("unsupported or invalid module descriptor");
      const auto module_entry = entries.find(*module_path);
      if (module_entry == entries.end() ||
          module_entry->second.size() != *module_size ||
          core::sha256(module_entry->second) != *module_hash ||
          !declared_paths.insert(*module_path).second)
        return validation_failure("module size, hash, or path mismatch");
      const auto module_json = as_string(module_entry->second);
      if (source_schema(module_json) != *module_schema)
        return validation_failure("module schema does not match manifest");
      switch (desc->type) {
      case SidecarKind::visual: {
        const auto parsed_visual =
            core::parse_visual_profile_json(module_json, *profile);
        const auto *visual = std::get_if<core::VisualProfile>(&parsed_visual);
        if (visual == nullptr)
          return validation_failure("visual module is invalid");
        item.visual_json = module_json;
        item.visual_profile = *visual;
        break;
      }
      case SidecarKind::automation: {
        const auto parsed_automation =
            core::parse_automation_profile_json(module_json, *profile);
        const auto *automation =
            std::get_if<core::AutomationProfile>(&parsed_automation);
        if (automation == nullptr)
          return validation_failure("automation module is invalid");
        item.automation_json = module_json;
        item.automation_profile = *automation;
        break;
      }
      case SidecarKind::companion: {
        const auto parsed_companion =
            core::parse_companion_profile_json(module_json, *profile);
        const auto *companion =
            std::get_if<core::CompanionProfile>(&parsed_companion);
        if (companion == nullptr)
          return validation_failure("companion module is invalid");
        item.companion_json = module_json;
        item.companion_profile = *companion;
        break;
      }
      }
      packed_schemas[std::string(*module_kind)] =
          static_cast<std::uint32_t>(*module_schema);
    }
    for (const auto &desc : kSidecars) {
      bool should_check = true;
      core::ProfileModuleKind module_kind{};
      switch (desc.type) {
      case SidecarKind::visual:
        should_check = *base_schema >= 3U;
        module_kind = core::ProfileModuleKind::visual;
        break;
      case SidecarKind::automation:
        should_check = *base_schema >= 4U;
        module_kind = core::ProfileModuleKind::automation;
        break;
      case SidecarKind::companion:
        module_kind = core::ProfileModuleKind::companion;
        break;
      }
      if (!should_check)
        continue;
      const auto declared = std::ranges::find(profile->modules, module_kind,
                                              &core::ProfileModule::kind);
      const bool declares = declared != profile->modules.end();
      const auto packed = packed_schemas.find(std::string(desc.kind));
      const bool has_packed = packed != packed_schemas.end();
      if (declares != has_packed ||
          (declares && declared->schema_version != packed->second))
        return validation_failure(std::string("base ") +
                                  std::string(desc.kind) +
                                  " metadata disagrees with pack modules");
    }
    std::set<std::string, std::less<>> declared_references;
    for (const auto &reference : references->as_array()) {
      if (!reference.is_string() ||
          !core::is_lowercase_sha256(reference.as_string()) ||
          !declared_references.insert(reference.as_string()).second ||
          !pack.assets.contains(reference.as_string()))
        return validation_failure(
            "profile contains an invalid asset reference");
      item.asset_sha256.push_back(reference.as_string());
    }
    const auto actual_references =
        item.visual_profile.has_value()
            ? visual_asset_references(*item.visual_profile)
            : std::vector<std::string>{};
    if (actual_references != item.asset_sha256)
      return validation_failure(
          "manifest assets do not match visual references");
    pack.profiles.push_back(std::move(item));
  }

  if (declared_paths.size() != entries.size())
    return validation_failure("pack contains undeclared files");
  return pack;
}

PackOperationResult
import_profile_pack(const ValidatedPack &pack,
                    const std::span<const ImportChoice> choices,
                    const std::filesystem::path &profile_directory) {
  if (choices.empty() || choices.size() > pack.profiles.size())
    return failure("select at least one valid pack profile");

  struct ExistingProfile {
    std::filesystem::path path;
    std::string id;
  };
  std::vector<ExistingProfile> existing;
  std::set<std::string, std::less<>> used_ids;
  std::set<std::wstring, std::less<>> used_filenames;
  std::error_code error;
  std::filesystem::create_directories(profile_directory, error);
  if (error)
    return failure("profile directory could not be created");
  for (std::filesystem::directory_iterator iterator(profile_directory, error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error)
      continue;
    if (_wcsicmp(iterator->path().extension().c_str(), L".json") != 0)
      continue;
    const auto folded = platform::try_casefold_path(
        platform::try_wide_to_utf8(iterator->path().filename().wstring())
            .value_or(std::string{}));
    if (!folded.has_value() || !used_filenames.insert(*folded).second)
      return failure("profile directory contains case-colliding filenames");
    const auto source = platform::read_profile_file(iterator->path());
    if (!source.has_value())
      continue;
    const auto parsed = core::parse_profile_json(*source);
    const auto *profile = std::get_if<core::GameProfile>(&parsed);
    if (profile == nullptr)
      continue;
    if (!used_ids.insert(profile->id).second)
      return failure("profile directory contains duplicate profile IDs");
    existing.push_back({.path = iterator->path(), .id = profile->id});
  }
  if (error)
    return failure("profile directory could not be enumerated");

  std::set<std::size_t> selected_indices;
  std::set<std::wstring, std::less<>> selected_targets;
  std::vector<platform::ProfileFileMutation> mutations;
  std::set<std::string, std::less<>> required_assets;
  const auto read_existing_bytes =
      [](const std::filesystem::path &path,
         const std::size_t limit) -> std::optional<std::vector<std::byte>> {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error)
      return std::nullopt;
    return read_file(path, limit);
  };
  const auto add_backup_mutation = [&](const std::filesystem::path &target,
                                       const std::size_t limit) -> bool {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(target, exists_error);
    if (exists_error)
      return false;
    if (!exists)
      return true;
    const auto previous = read_existing_bytes(target, limit);
    if (!previous.has_value())
      return false;
    auto backup = target;
    backup += L".bak";
    mutations.push_back(
        {.target = std::move(backup), .contents = std::move(previous)});
    return true;
  };

  for (const auto &choice : choices) {
    if (choice.profile_index >= pack.profiles.size() ||
        choice.action == ImportAction::skip ||
        !selected_indices.insert(choice.profile_index).second)
      return failure("import choices are invalid or duplicated");
    auto imported = pack.profiles[choice.profile_index];
    const auto resolved_suggested =
        resolve_suggested_profile_filename(imported.suggested_filename);
    if (!resolved_suggested.has_value())
      return failure("suggested profile filename is invalid");
    const auto suggested = *resolved_suggested;

    const auto by_id =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          return candidate.id == imported.profile.id;
        });
    const auto suggested_folded = platform::try_casefold_path(
        platform::try_wide_to_utf8(suggested.wstring())
            .value_or(std::string{}));
    if (!suggested_folded.has_value())
      return failure("suggested profile filename is invalid");
    const auto by_filename =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          const auto folded = platform::try_casefold_path(
              platform::try_wide_to_utf8(candidate.path.filename().wstring())
                  .value_or(std::string{}));
          return folded.has_value() && *folded == *suggested_folded;
        });

    std::filesystem::path target;
    if (choice.action == ImportAction::copy) {
      const auto base_id = imported.profile.id + "-copy";
      imported.profile.id = base_id;
      for (std::uint32_t suffix = 2; used_ids.contains(imported.profile.id);
           ++suffix)
        imported.profile.id = base_id + "-" + std::to_string(suffix);
      if (imported.visual_profile.has_value())
        imported.visual_profile->profile_id = imported.profile.id;
      if (imported.automation_profile.has_value())
        imported.automation_profile->profile_id = imported.profile.id;
      if (imported.companion_profile.has_value())
        imported.companion_profile->profile_id = imported.profile.id;

      const auto stem = suggested.stem().wstring() + L" copy";
      target = profile_directory / (stem + L".json");
      for (std::uint32_t suffix = 2;; ++suffix) {
        const auto folded = platform::try_casefold_path(
            platform::try_wide_to_utf8(target.filename().wstring())
                .value_or(std::string{}));
        if (folded.has_value() && !used_filenames.contains(*folded))
          break;
        target = profile_directory /
                 (stem + L" " + std::to_wstring(suffix) + L".json");
      }
    } else {
      if (by_id != existing.end() && by_filename != existing.end() &&
          by_id->path != by_filename->path)
        return failure("profile ID and filename conflict with different files; "
                       "import a copy instead");
      if (by_id != existing.end())
        target = by_id->path;
      else if (by_filename != existing.end())
        target = by_filename->path;
      else if (used_filenames.contains(*suggested_folded))
        return failure("suggested filename belongs to an unreadable or invalid "
                       "profile; import a copy instead");
      else
        target = profile_directory / suggested;
    }
    const auto target_folded = platform::try_casefold_path(
        platform::try_wide_to_utf8(target.filename().wstring())
            .value_or(std::string{}));
    if (!target_folded.has_value() ||
        !selected_targets.insert(*target_folded).second)
      return failure(
          "multiple selected profiles resolve to the same output file");
    used_filenames.insert(*target_folded);
    used_ids.insert(imported.profile.id);

    imported.profile.schema_version = 4;
    imported.profile.modules.clear();
    for (const auto &desc : kSidecars) {
      switch (desc.type) {
      case SidecarKind::visual:
        if (imported.visual_profile.has_value())
          imported.profile.modules.push_back(
              {.kind = core::ProfileModuleKind::visual,
               .schema_version = imported.visual_profile->schema_version});
        break;
      case SidecarKind::automation:
        if (imported.automation_profile.has_value())
          imported.profile.modules.push_back(
              {.kind = core::ProfileModuleKind::automation,
               .schema_version = imported.automation_profile->schema_version});
        break;
      case SidecarKind::companion:
        if (imported.companion_profile.has_value())
          imported.profile.modules.push_back(
              {.kind = core::ProfileModuleKind::companion,
               .schema_version = imported.companion_profile->schema_version});
        break;
      }
    }
    const auto base_json = core::serialize_profile_json(imported.profile);
    if (!add_backup_mutation(target, core::max_profile_json_bytes))
      return failure("existing profile could not be protected by a backup");
    mutations.push_back({.target = target, .contents = as_bytes(base_json)});
    auto stage_sidecar = [&](const std::filesystem::path &sidecar_path,
                             const auto &profile, const std::string_view kind,
                             const std::size_t max_bytes,
                             auto serialize) -> std::optional<PackError> {
      auto state_path = sidecar_path;
      state_path += L".state";
      auto backup_path = sidecar_path;
      backup_path += L".bak";
      if (profile.has_value()) {
        const auto json = serialize(*profile);
        if (!add_backup_mutation(sidecar_path, max_bytes))
          return PackError{"existing " + std::string(kind) +
                           " sidecar could not be protected by a backup"};
        mutations.push_back(
            {.target = sidecar_path, .contents = as_bytes(json)});
        mutations.push_back(
            {.target = state_path, .contents = as_bytes("active\n")});
      } else {
        std::error_code error;
        const bool has_stale =
            platform::has_stale_sidecar_markers(sidecar_path, error);
        if (error)
          return PackError{"existing " + std::string(kind) +
                           " sidecar state could not be inspected"};
        if (has_stale) {
          mutations.push_back(
              {.target = sidecar_path, .contents = std::nullopt});
          mutations.push_back(
              {.target = backup_path, .contents = std::nullopt});
          mutations.push_back(
              {.target = state_path, .contents = as_bytes("removed\n")});
        }
      }
      return std::nullopt;
    };
    for (const auto &desc : kSidecars) {
      std::optional<PackError> sidecar_error;
      switch (desc.type) {
      case SidecarKind::visual:
        sidecar_error = stage_sidecar(
            platform::visual_sidecar_path(target), imported.visual_profile,
            desc.kind, desc.max_bytes, core::serialize_visual_profile_json);
        if (imported.visual_profile.has_value())
          for (const auto &hash : imported.asset_sha256)
            required_assets.insert(hash);
        break;
      case SidecarKind::automation:
        sidecar_error = stage_sidecar(platform::automation_sidecar_path(target),
                                      imported.automation_profile, desc.kind,
                                      desc.max_bytes,
                                      core::serialize_automation_profile_json);
        break;
      case SidecarKind::companion:
        sidecar_error =
            stage_sidecar(platform::companion_sidecar_path(target),
                          imported.companion_profile, desc.kind, desc.max_bytes,
                          core::serialize_companion_profile_json);
        break;
      }
      if (sidecar_error)
        return *sidecar_error;
    }
  }

  // Immutable assets publish before profile documents. Existing content must
  // already match its content-addressed name.
  std::vector<platform::ProfileFileMutation> asset_mutations;
  for (const auto &hash : required_assets) {
    const auto found = pack.assets.find(hash);
    if (found == pack.assets.end())
      return failure("selected profile asset is missing");
    const auto target = asset_path_for_profile_root(profile_directory, hash);
    std::error_code asset_error;
    if (std::filesystem::exists(target, asset_error)) {
      const auto existing_bytes = read_file(target, max_pack_asset_bytes);
      if (asset_error || !existing_bytes.has_value() ||
          core::sha256(*existing_bytes) != hash)
        return failure("existing content-addressed asset is corrupt: " + hash);
      continue;
    }
    if (asset_error)
      return failure("asset destination could not be inspected");
    asset_mutations.push_back(
        {.target = target, .contents = found->second.bytes});
  }
  asset_mutations.insert(asset_mutations.end(),
                         std::make_move_iterator(mutations.begin()),
                         std::make_move_iterator(mutations.end()));
  if (!platform::commit_profile_transaction(profile_directory, asset_mutations))
    return failure("profile-pack transaction failed and was rolled back");
  // LayerD may legitimately not be running. The committed files are the
  // source of truth and will be loaded at its next startup.
  (void)platform::signal_profile_reload();
  return std::monostate{};
}

PackOperationResult recover_profile_pack_transactions(
    const std::filesystem::path &profile_directory) {
  return platform::recover_profile_transactions(profile_directory)
             ? PackOperationResult{std::monostate{}}
             : failure("an interrupted profile-pack transaction could not be "
                       "recovered");
}

} // namespace layerc::bundle
