#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "pack_detail.hpp"
#include "pack_sidecar_table.hpp"

#include "layerc/platform/text.hpp"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <system_error>
#include <type_traits>
#include <utility>

namespace layerc::bundle {
namespace json = core::json;
namespace {

using detail::as_string;
using detail::asset_path_for_profile;
using detail::EntryMap;
using detail::failure;
using detail::manifest_json;
using detail::safe_entry_path;
using detail::SidecarPathLists;
using detail::validation_failure;
using detail::visual_asset_references;

std::vector<std::byte> owned_bytes(const std::string_view source) {
  const auto bytes =
      std::as_bytes(std::span(source.data(), source.size()));
  return {bytes.begin(), bytes.end()};
}

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
                             std::string &out_json,
                             const Reserve &reserve_entry) {
  auto json = read_fn(sidecar_source);
  if (!json.has_value())
    return PackError{std::string(kind) +
                     " sidecar could not be read: " + profile_path.string()};
  if (!reserve_entry(json->size()))
    return PackError{"pack would exceed 256 entries or 128 MiB uncompressed"};
  out_json = std::move(*json);
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

// Path <-> UTF-8 conversions live in platform/text.hpp (path_to_utf8 /
// path_from_utf8) — this TU uses the canonical pair so failure modes match
// the transaction engine and store scan.
bool commit_pack_transaction(
    const std::filesystem::path &profile_directory,
    const std::span<const platform::ProfileFileMutation> mutations) {
  const auto roots = platform::profile_transaction_scope(profile_directory);
  return roots.has_value() &&
         platform::commit_profile_transaction(*roots, mutations);
}

bool recover_pack_transaction(
    const std::filesystem::path &profile_directory) {
  const auto roots = platform::profile_transaction_scope(profile_directory);
  return roots.has_value() && platform::recover_profile_transactions(*roots);
}

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
  const auto sanitized = platform::try_wide_to_utf8(*filename);
  if (!sanitized.has_value()) return std::nullopt;
  const auto decoded = platform::path_from_utf8(*sanitized);
  if (!decoded.has_value()) return std::nullopt;
  std::filesystem::path resolved = *decoded;
  if (!platform::has_json_extension(resolved)) return std::nullopt;
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
            if (auto error = sidecar_export_error(sidecar, path, desc.kind))
              return error;
            if (sidecar.state != platform::ModuleSidecarState::valid ||
                !sidecar.profile.has_value())
              return std::nullopt;
            std::string module_json;
            if (auto error = load_sidecar_json_for_export(
                    sidecar.source_path, path, desc.kind, Traits::read,
                    module_json, reserve_entry))
              return error;
            payload = ModulePayload<SidecarProfile>{
                .json = std::move(module_json), .profile = *sidecar.profile};
            if constexpr (std::is_same_v<SidecarProfile,
                                         core::VisualProfile>) {
              item.asset_sha256 = visual_asset_references(*sidecar.profile);
              for (const auto &hash : item.asset_sha256) {
                if (assets.contains(hash)) continue;
                auto bytes = platform::read_durable_file(
                    asset_path_for_profile(path, hash), max_pack_asset_bytes);
                if (!bytes.has_value() || core::sha256(*bytes) != hash)
                  return PackError{"visual asset is missing or corrupt: " +
                                   hash};
                if (!reserve_entry(bytes->size()))
                  return PackError{
                      "pack would exceed 256 entries or 128 MiB uncompressed"};
                assets.try_emplace(
                    hash,
                    PackAsset{.sha256 = hash, .bytes = std::move(*bytes)});
              }
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
            stage_module(module, sidecar_paths[desc.index][index], desc.suffix);
          });
    }
  }
  for (const auto &[hash, asset] : assets)
    entries.emplace("assets/" + hash + ".png", asset.bytes);
  const auto manifest =
      manifest_json(profiles, assets, base_paths, sidecar_paths);
  if (!manifest.has_value())
    return failure("could not hash profile data for the pack manifest");
  if (manifest->size() > max_pack_bytes - aggregate_bytes)
    return failure("pack would exceed 128 MiB uncompressed");

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 128U * 1024U))
    return failure("could not initialize ZIP writer");
  bool ok = add_zip_entry(zip, "manifest.json", owned_bytes(*manifest));
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
                 : failure("pack file could not be written");
}

namespace {

using ArchiveValidationResult = std::variant<EntryMap, PackError>;

ArchiveValidationResult
validate_archive_entries(const std::filesystem::path &source) {
  const auto archive_bytes =
      platform::read_durable_file(source, max_pack_bytes);
  if (!archive_bytes.has_value())
    return PackError{"pack could not be read or exceeds 128 MiB"};

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, archive_bytes->data(),
                              archive_bytes->size(), 0))
    return PackError{"file is not a valid ZIP archive"};
  const auto close_zip = [](mz_zip_archive *archive) {
    (void)mz_zip_reader_end(archive);
  };
  std::unique_ptr<mz_zip_archive, decltype(close_zip)> zip_guard{&zip,
                                                                 close_zip};
  if (mz_zip_is_zip64(&zip)) return PackError{"ZIP64 packs are not supported"};
  const auto count = mz_zip_reader_get_num_files(&zip);
  if (count == 0 || count > max_pack_entries)
    return PackError{"pack must contain between 1 and 256 entries"};

  EntryMap entries;
  std::set<std::string, std::less<>> folded_paths;
  std::uint64_t total_uncompressed{};
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, index, &stat))
      return PackError{"pack central directory is invalid"};
    if (stat.m_is_directory)
      return PackError{"directory entries are not allowed"};
    if (stat.m_is_encrypted || !stat.m_is_supported)
      return PackError{"encrypted or unsupported ZIP entries are not allowed"};
    const auto unix_mode = (stat.m_external_attr >> 16U) & 0170000U;
    constexpr mz_uint32 reparse_point_attribute = 0x00000400U;
    if (unix_mode == 0120000U ||
        (stat.m_external_attr & reparse_point_attribute) != 0U)
      return PackError{"symlink or reparse entries are not allowed"};
    const auto filename_size =
        mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (filename_size <= 1U || filename_size > 4096U)
      return PackError{"pack entry name is empty or too long"};
    std::string path(static_cast<std::size_t>(filename_size) - 1U, '\0');
    if (mz_zip_reader_get_filename(&zip, index, path.data(), filename_size) !=
        filename_size)
      return PackError{"pack entry name could not be read"};
    if (!safe_entry_path(path))
      return PackError{"unsafe pack entry path: " + path};
    const auto folded = platform::try_casefold_path(path);
    if (!folded.has_value() || !folded_paths.insert(*folded).second)
      return PackError{"duplicate or case-colliding pack entry: " + path};
    if (stat.m_uncomp_size > max_pack_bytes ||
        total_uncompressed > max_pack_bytes - stat.m_uncomp_size)
      return PackError{"pack expands beyond 128 MiB"};
    total_uncompressed += stat.m_uncomp_size;
    std::vector<std::byte> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(),
                                      0))
      return PackError{"pack entry failed CRC or decompression: " + path};
    entries.emplace(std::move(path), std::move(bytes));
  }
  if (!mz_zip_validate_archive(&zip, 0))
    return PackError{"ZIP archive validation failed"};
  return entries;
}

} // namespace

namespace {

PackValidationResult validate_profile_descriptors(
    const core::json::Value &profile_values, const EntryMap &entries,
    ValidatedPack pack, std::set<std::string, std::less<>> declared_paths) {
  json::Reader fields;
  const auto invalid = [&](std::string message) {
    if (!fields.ok()) message += ": " + fields.error().describe();
    return validation_failure(std::move(message));
  };
  std::set<std::string, std::less<>> profile_ids;
  std::size_t profile_index{};
  for (const auto &value : profile_values.as_array()) {
    const auto descriptor_path = "$.profiles[" + std::to_string(profile_index++) + "]";
    if (!fields.exact_fields(value,
                      {"id", "suggestedFilename", "base", "modules", "assets"}, descriptor_path))
      return invalid(
          "profile descriptor has unknown or missing fields");
    const auto id = fields.string(value, "id", descriptor_path, json::Required::yes, json::EmptyString::allow);
    const auto suggested = fields.string(value, "suggestedFilename", descriptor_path, json::Required::yes, json::EmptyString::allow);
    const auto *base = fields.field(value, "base", descriptor_path, json::Required::yes);
    const auto *modules = fields.field(value, "modules", descriptor_path, json::Required::yes);
    const auto *references = fields.field(value, "assets", descriptor_path, json::Required::yes);
    if (!id.has_value() || id->empty() || !profile_ids.insert(*id).second ||
        !suggested.has_value() || suggested->empty() ||
        !safe_entry_path(*suggested) || base == nullptr || modules == nullptr ||
        !modules->is_array() || references == nullptr ||
        !references->is_array())
      return invalid("profile descriptor identity is invalid");
    if (!fields.exact_fields(*base, {"path", "schemaVersion", "size", "sha256"}, descriptor_path + ".base"))
      return invalid("base profile descriptor is invalid");
    const auto base_path = fields.string(*base, "path", descriptor_path + ".base", json::Required::yes, json::EmptyString::allow);
    const auto base_schema = fields.unsigned_integer(*base, "schemaVersion", descriptor_path + ".base", UINT64_MAX, json::Required::yes);
    const auto base_size = fields.unsigned_integer(*base, "size", descriptor_path + ".base", UINT64_MAX, json::Required::yes);
    const auto base_hash = fields.string(*base, "sha256", descriptor_path + ".base", json::Required::yes, json::EmptyString::allow);
    if (!base_path.has_value() || !base_schema.has_value() ||
        *base_schema < 1U || *base_schema > 4U || !base_size.has_value() ||
        *base_size > core::max_profile_json_bytes || !base_hash.has_value() ||
        !core::is_lowercase_sha256(*base_hash))
      return invalid("base profile descriptor fields are invalid");
    const auto base_entry = entries.find(*base_path);
    if (base_entry == entries.end() ||
        base_entry->second.size() != *base_size ||
        core::sha256(base_entry->second) != *base_hash ||
        !declared_paths.insert(*base_path).second)
      return invalid("base profile size, hash, or path mismatch");
    const auto base_json = as_string(base_entry->second);
    const auto base_document = json::parse(base_json);
    const auto *base_root = std::get_if<json::Value>(&base_document);
    if (!base_root)
      return invalid("base profile JSON is invalid");
    if (fields.unsigned_integer(*base_root, "schemaVersion",
                                descriptor_path + ".baseDocument", UINT32_MAX,
                                json::Required::yes) != *base_schema)
      return invalid("base profile schema does not match manifest");
    const auto parsed_profile = core::parse_profile_value(*base_root);
    const auto *profile = std::get_if<core::GameProfile>(&parsed_profile);
    if (profile == nullptr || profile->id != *id)
      return invalid(
          "base profile is invalid or has a mismatched ID");

    PackProfile item;
    item.suggested_filename = *suggested;
    item.base_json = base_json;
    item.profile = *profile;
    std::map<std::string, std::uint32_t, std::less<>> packed_schemas;
    if (modules->as_array().size() > kSidecars.size())
      return invalid(
          "at most one visual, automation, and companion module are supported");
    std::set<std::string, std::less<>> module_kinds;
    std::size_t module_index{};
    for (const auto &module : modules->as_array()) {
      const auto module_descriptor_path =
          descriptor_path + ".modules[" + std::to_string(module_index++) + "]";
      if (!fields.exact_fields(module,
                        {"kind", "path", "schemaVersion", "size", "sha256"}, module_descriptor_path))
        return invalid(
            "module descriptor has unknown or missing fields");
      const auto module_kind = fields.string(module, "kind", module_descriptor_path, json::Required::yes, json::EmptyString::allow);
      const auto module_path = fields.string(module, "path", module_descriptor_path, json::Required::yes, json::EmptyString::allow);
      const auto module_schema = fields.unsigned_integer(module, "schemaVersion", module_descriptor_path, UINT64_MAX, json::Required::yes);
      const auto module_size = fields.unsigned_integer(module, "size", module_descriptor_path, UINT64_MAX, json::Required::yes);
      const auto module_hash = fields.string(module, "sha256", module_descriptor_path, json::Required::yes, json::EmptyString::allow);
      if (!module_kind.has_value() ||
          !module_kinds.insert(*module_kind).second ||
          !module_path.has_value() || !module_schema.has_value() ||
          *module_schema < 1U || !module_size.has_value() ||
          !module_hash.has_value() || !core::is_lowercase_sha256(*module_hash))
        return invalid("unsupported or invalid module descriptor");
      const auto desc = sidecar_desc_for_kind(*module_kind);
      if (!desc.has_value() || *module_schema > desc->max_schema ||
          *module_size > desc->max_bytes)
        return invalid("unsupported or invalid module descriptor");
      const auto module_entry = entries.find(*module_path);
      if (module_entry == entries.end() ||
          module_entry->second.size() != *module_size ||
          core::sha256(module_entry->second) != *module_hash ||
          !declared_paths.insert(*module_path).second)
        return invalid("module size, hash, or path mismatch");
      const auto module_json = as_string(module_entry->second);
      const auto module_document = json::parse(module_json);
      const auto *module_root = std::get_if<json::Value>(&module_document);
      if (!module_root)
        return invalid("module JSON is invalid");
      if (fields.unsigned_integer(*module_root, "schemaVersion",
                                  module_descriptor_path + ".document", UINT32_MAX,
                                  json::Required::yes) != *module_schema)
        return invalid("module schema does not match manifest");
      const bool parsed_sidecar = detail::visit_sidecar(
          item, *desc, [&](auto &payload, const auto &traits) {
            using Traits = std::remove_cvref_t<decltype(traits)>;
            using SidecarProfile = typename Traits::Profile;
            auto parsed = Traits::parse(*module_root, *profile);
            const auto *sidecar = std::get_if<SidecarProfile>(&parsed);
            if (sidecar == nullptr) return false;
            payload = ModulePayload<SidecarProfile>{.json = module_json,
                                                    .profile = *sidecar};
            return true;
          });
      if (!parsed_sidecar)
        return invalid(std::string(desc->kind) + " module is invalid");
      packed_schemas[std::string(*module_kind)] =
          static_cast<std::uint32_t>(*module_schema);
    }
    for (const auto &desc : kSidecars) {
      if (*base_schema < desc.minimum_profile_schema) continue;
      const auto declared = std::ranges::find(profile->modules, desc.module_kind,
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
    const auto actual_references = item.visual.has_value()
                                       ? visual_asset_references(
                                             item.visual->profile)
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

} // namespace

PackValidationResult
validate_profile_pack(const std::filesystem::path &source) {
  json::Reader fields;
  const auto invalid = [&](std::string message) {
    if (!fields.ok()) message += ": " + fields.error().describe();
    return validation_failure(std::move(message));
  };
  auto archive = validate_archive_entries(source);
  if (const auto *error = std::get_if<PackError>(&archive)) return *error;
  auto entries = std::move(std::get<EntryMap>(archive));

  const auto manifest_entry = entries.find("manifest.json");
  if (manifest_entry == entries.end() ||
      manifest_entry->second.size() > core::max_profile_json_bytes)
    return invalid("manifest.json is missing or exceeds 256 KiB");
  const auto manifest_source = as_string(manifest_entry->second);
  const auto parsed_manifest = core::json::parse(manifest_source);
  const auto *manifest = std::get_if<core::json::Value>(&parsed_manifest);
  if (manifest == nullptr || !manifest->is_object() ||
      !fields.exact_fields(*manifest, {"schemaVersion", "kind", "producerVersion",
                                "profiles", "assets"}, "$"))
    return invalid("manifest root is invalid or has unknown fields");
  const auto manifest_schema = fields.unsigned_integer(*manifest, "schemaVersion", "$", UINT64_MAX, json::Required::yes);
  const auto kind = fields.string(*manifest, "kind", "$", json::Required::yes, json::EmptyString::allow);
  const auto producer = fields.string(*manifest, "producerVersion", "$", json::Required::yes, json::EmptyString::allow);
  const auto *profile_values = fields.field(*manifest, "profiles", "$", json::Required::yes);
  const auto *asset_values = fields.field(*manifest, "assets", "$", json::Required::yes);
  if (manifest_schema != 1U || kind != "layerc-profile-pack" ||
      !producer.has_value() || profile_values == nullptr ||
      !profile_values->is_array() || asset_values == nullptr ||
      !asset_values->is_array())
    return invalid(
        "manifest identity or collection fields are invalid");
  if (profile_values->as_array().empty() ||
      profile_values->as_array().size() > max_pack_profiles)
    return invalid(
        "manifest must declare between 1 and 64 profiles");

  ValidatedPack pack;
  pack.producer_version = *producer;
  std::set<std::string, std::less<>> declared_paths{"manifest.json"};
  std::size_t asset_index{};
  for (const auto &value : asset_values->as_array()) {
    const auto descriptor_path = "$.assets[" + std::to_string(asset_index++) + "]";
    if (!fields.exact_fields(value, {"sha256", "path", "size"}, descriptor_path))
      return invalid(
          "asset descriptor has unknown or missing fields");
    const auto hash = fields.string(value, "sha256", descriptor_path, json::Required::yes, json::EmptyString::allow);
    const auto path = fields.string(value, "path", descriptor_path, json::Required::yes, json::EmptyString::allow);
    const auto size = fields.unsigned_integer(value, "size", descriptor_path, UINT64_MAX, json::Required::yes);
    if (!hash.has_value() || !core::is_lowercase_sha256(*hash) ||
        !path.has_value() || !size.has_value() ||
        *size > max_pack_asset_bytes || *path != "assets/" + *hash + ".png")
      return invalid("asset descriptor is invalid");
    const auto found = entries.find(*path);
    if (found == entries.end() || found->second.size() != *size ||
        core::sha256(found->second) != *hash)
      return invalid("asset size or SHA-256 mismatch: " + *hash);
    if (!declared_paths.insert(*path).second ||
        !pack.assets
             .emplace(*hash, PackAsset{.sha256 = *hash, .bytes = found->second})
             .second)
      return invalid("duplicate asset declaration: " + *hash);
  }

  return validate_profile_descriptors(*profile_values, entries, std::move(pack),
                                      std::move(declared_paths));
}

namespace {

struct ImportPlan {
  std::vector<platform::ProfileFileMutation> mutations;
  std::map<std::filesystem::path, std::string> required_assets;
  std::vector<ResolvedImportEntry> entries;
};

using ImportPlanResult = std::variant<ImportPlan, PackError>;

ImportPlanResult plan_failure(std::string message) {
  return PackError{std::move(message)};
}

ImportPlanResult build_import_plan(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory,
    const bool keep_existing = false) {
  if (choices.empty() || choices.size() > pack.profiles.size())
    return plan_failure("select at least one valid pack profile");

  struct ExistingProfile {
    std::filesystem::path path;
    std::string id;
  };
  std::vector<ExistingProfile> existing;
  std::set<std::string, std::less<>> used_ids;
  std::set<std::string, std::less<>> used_filenames;
  const auto scan = platform::scan_profile_directory(profile_directory);
  if (!scan.error.empty()) return plan_failure(scan.error);
  for (const auto &path : scan.base_paths) {
    const auto folded = platform::try_casefold_path(
        platform::path_to_utf8(path.filename()).value_or(std::string{}));
    if (!folded.has_value() || !used_filenames.insert(*folded).second)
      return plan_failure("profile directory contains case-colliding filenames");
  }
  for (const auto &stored : scan.profiles) {
    if (!used_ids.insert(stored.profile.id).second)
      return plan_failure("profile directory contains duplicate profile IDs");
    existing.push_back({.path = stored.path, .id = stored.profile.id});
  }

  std::vector<ResolvedImportEntry> resolved_entries;
  std::set<std::size_t> selected_indices;
  std::set<std::string, std::less<>> selected_targets;
  std::vector<platform::ProfileFileMutation> mutations;
  std::map<std::filesystem::path, std::string> required_assets;
  const auto add_backup_mutation = [&](const std::filesystem::path &target,
                                       const std::size_t limit) -> bool {
    auto previous = platform::read_durable_file(target, limit);
    if (previous.status == platform::FileReadStatus::missing) return true;
    if (!previous.has_value()) return false;
    mutations.push_back(
        {.target = platform::recovery_backup_path(target),
         .contents = std::move(previous.bytes)});
    return true;
  };

  for (const auto &choice : choices) {
    if (choice.profile_index >= pack.profiles.size() ||
        !selected_indices.insert(choice.profile_index).second)
      return plan_failure("import choices are invalid or duplicated");
    // --keep-existing maps to skip: the entry is validated but plans no
    // mutations, so mixed existing/new batches keep old entries while
    // importing new ones instead of failing the whole batch.
    if (choice.action == ImportAction::skip) continue;
    auto imported = pack.profiles[choice.profile_index];
    const auto resolved_suggested =
        resolve_suggested_profile_filename(imported.suggested_filename);
    if (!resolved_suggested.has_value())
      return plan_failure("suggested profile filename is invalid");
    const auto suggested = *resolved_suggested;

    const auto by_id =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          return candidate.id == imported.profile.id;
        });
    const auto suggested_folded = platform::try_casefold_path(
        platform::path_to_utf8(suggested).value_or(std::string{}));
    if (!suggested_folded.has_value())
      return plan_failure("suggested profile filename is invalid");
    const auto by_filename =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          const auto folded = platform::try_casefold_path(
              platform::path_to_utf8(candidate.path.filename())
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
      if (imported.visual.has_value())
        imported.visual->profile.profile_id = imported.profile.id;
      if (imported.automation.has_value())
        imported.automation->profile.profile_id = imported.profile.id;
      if (imported.companion.has_value())
        imported.companion->profile.profile_id = imported.profile.id;

      const auto stem =
          platform::path_to_utf8(suggested.stem()).value_or(std::string{}) +
          " copy";
      const auto copy_path = platform::path_from_utf8(stem + ".json");
      if (!copy_path.has_value())
        return plan_failure("suggested filename is not valid UTF-8");
      target = profile_directory / *copy_path;
      for (std::uint32_t suffix = 2;; ++suffix) {
        const auto folded = platform::try_casefold_path(
            platform::path_to_utf8(target.filename())
                .value_or(std::string{}));
        if (folded.has_value() && !used_filenames.contains(*folded)) break;
        const auto suffixed = platform::path_from_utf8(
            stem + " " + std::to_string(suffix) + ".json");
        if (!suffixed.has_value())
          return plan_failure("suggested filename is not valid UTF-8");
        target = profile_directory / *suffixed;
      }
    } else {
      if (by_id != existing.end() && by_filename != existing.end() &&
          by_id->path != by_filename->path)
        return plan_failure("profile ID and filename conflict with different files; "
                       "import a copy instead");
      if (by_id != existing.end())
        target = by_id->path;
      else if (by_filename != existing.end())
        target = by_filename->path;
      else if (used_filenames.contains(*suggested_folded))
        return plan_failure("suggested filename belongs to an unreadable or invalid "
                       "profile; import a copy instead");
      else
        target = profile_directory / suggested;
    }
    if (choice.expected_target.has_value() &&
        choice.expected_target->lexically_normal() != target.lexically_normal())
      return plan_failure("import destination changed; review the import again");
    const bool collides = by_id != existing.end() ||
                          used_filenames.contains(*suggested_folded);
    const bool skipped = keep_existing && collides;
    resolved_entries.push_back({.profile_index = choice.profile_index,
                                .id = imported.profile.id,
                                .target = target,
                                .collides = collides,
                                .skipped = skipped});
    if (skipped) continue;
    const auto target_folded = platform::try_casefold_path(
        platform::path_to_utf8(target.filename()).value_or(std::string{}));
    if (!target_folded.has_value() ||
        !selected_targets.insert(*target_folded).second)
      return plan_failure(
          "multiple selected profiles resolve to the same output file");
    used_filenames.insert(*target_folded);
    used_ids.insert(imported.profile.id);

    imported.profile.schema_version = 4;
    imported.profile.modules.clear();
    for (const auto &desc : kSidecars) {
      detail::visit_sidecar(
          imported, desc, [&](const auto &module, const auto &) {
            if (module.has_value())
              imported.profile.modules.push_back(
                  {.kind = desc.module_kind,
                   .schema_version = module->profile.schema_version});
          });
    }
    const auto base_json = core::serialize_profile_json(imported.profile);
    if (!add_backup_mutation(target, core::max_profile_json_bytes))
      return plan_failure("existing profile could not be protected by a backup");
    mutations.push_back({.target = target,
                         .contents = owned_bytes(base_json),
                         .must_be_absent = choice.require_absent ||
                             choice.action == ImportAction::copy || !collides});
    auto stage_sidecar = [&](const platform::SidecarPaths &paths,
                             const auto &module, const std::string_view kind,
                             const std::size_t max_bytes,
                             auto serialize) -> std::optional<PackError> {
      if (module.has_value()) {
        const auto json = serialize(module->profile);
        if (!add_backup_mutation(paths.primary, max_bytes))
          return PackError{"existing " + std::string(kind) +
                           " sidecar could not be protected by a backup"};
        mutations.push_back(
            {.target = paths.primary, .contents = owned_bytes(json)});
        mutations.push_back({.target = paths.marker,
                             .contents = owned_bytes(
                                  platform::sidecar_marker_contents(true))});
      } else {
        const auto markers =
            platform::inspect_sidecar_markers(paths.primary);
        if (markers.error)
          return PackError{"existing " + std::string(kind) +
                           " sidecar state could not be inspected"};
        if (markers.present) {
          mutations.push_back(
              {.target = paths.primary, .contents = std::nullopt});
          mutations.push_back(
              {.target = paths.backup, .contents = std::nullopt});
          mutations.push_back({.target = paths.marker,
                               .contents = owned_bytes(
                                    platform::sidecar_marker_contents(false))});
        }
      }
      return std::nullopt;
    };
    for (const auto &desc : kSidecars) {
      const auto sidecar_error = detail::visit_sidecar(
          imported, desc,
          [&](const auto &module,
              const auto &traits) -> std::optional<PackError> {
            using Traits = std::remove_cvref_t<decltype(traits)>;
            const auto error = stage_sidecar(
                platform::sidecar_paths(target, desc.platform_kind), module,
                desc.kind, desc.max_bytes, Traits::serialize);
            if constexpr (std::is_same_v<typename Traits::Profile,
                                         core::VisualProfile>) {
              if (module.has_value())
                for (const auto &hash : imported.asset_sha256)
                  required_assets.emplace(asset_path_for_profile(target, hash),
                                          hash);
            }
            return error;
          });
      if (sidecar_error) return *sidecar_error;
    }
  }


  return ImportPlan{.mutations = std::move(mutations),
                    .required_assets = std::move(required_assets),
                    .entries = std::move(resolved_entries)};
}

} // namespace

PackOperationResult
import_profile_pack(const ValidatedPack &pack,
                    const std::span<const ImportChoice> choices,
                    const std::filesystem::path &profile_directory) {
  auto built = build_import_plan(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&built)) return *error;
  auto plan = std::move(std::get<ImportPlan>(built));
  // Immutable assets publish before profile documents. Existing content must
  // already match its content-addressed name.
  std::vector<platform::ProfileFileMutation> asset_mutations;
  for (const auto &[target, hash] : plan.required_assets) {
    const auto found = pack.assets.find(hash);
    if (found == pack.assets.end())
      return failure("selected profile asset is missing");
    if (target.empty()) return failure("asset destination is invalid");
    const auto existing_bytes =
        platform::read_durable_file(target, max_pack_asset_bytes);
    if (existing_bytes.has_value()) {
      if (core::sha256(*existing_bytes) != hash)
        return failure("existing content-addressed asset is corrupt: " + hash);
      continue;
    }
    if (existing_bytes.status != platform::FileReadStatus::missing)
      return failure("asset destination could not be read");
    asset_mutations.push_back(
        {.target = target, .contents = found->second.bytes});
  }
  asset_mutations.insert(asset_mutations.end(),
                         std::make_move_iterator(plan.mutations.begin()),
                         std::make_move_iterator(plan.mutations.end()));
  if (asset_mutations.empty()) return std::monostate{};
  if (!commit_pack_transaction(profile_directory, asset_mutations))
    return failure("profile-pack transaction failed; recovery may be required");
  // LayerD may legitimately not be running. The committed files are the
  // source of truth and will be loaded at its next startup.
  (void)platform::signal_profile_reload();
  return std::monostate{};
}

PackOperationResult recover_profile_pack_transactions(
    const std::filesystem::path &profile_directory) {
  return recover_pack_transaction(profile_directory)
             ? PackOperationResult{std::monostate{}}
             : failure("an interrupted profile-pack transaction could not be "
                       "recovered");
}

ImportPreview preview_profile_import(
    const ValidatedPack &pack, const bool keep_existing,
    const std::filesystem::path &profile_directory) {
  std::vector<ImportChoice> choices;
  choices.reserve(pack.profiles.size());
  for (std::size_t index = 0; index < pack.profiles.size(); ++index)
    choices.push_back({.profile_index = index, .action = ImportAction::replace});
  auto built = build_import_plan(pack, choices, profile_directory, keep_existing);
  if (const auto *error = std::get_if<PackError>(&built))
    return {.entries = {}, .error = *error};
  return {.entries = std::move(std::get<ImportPlan>(built).entries), .error = std::nullopt};
}

ImportPreview preview_profile_import(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  auto built = build_import_plan(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&built))
    return {.entries = {}, .error = *error};
  return {.entries = std::move(std::get<ImportPlan>(built).entries),
          .error = std::nullopt};
}

std::optional<PackError> validate_import_choices(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  auto built = build_import_plan(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&built)) return *error;
  return std::nullopt;
}

} // namespace layerc::bundle
