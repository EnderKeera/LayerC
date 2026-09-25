// Profile-pack validation: archive safety, manifest descriptors, and documents.
#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/text.hpp"
#include "pack_detail.hpp"
#include "pack_sidecar_table.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <set>
#include <type_traits>
#include <utility>

namespace layerc::bundle {
namespace json = core::json;
namespace {

using core::as_text;
using detail::EntryMap;
using detail::kSidecars;
using detail::pack_error;
using detail::safe_entry_path;
using detail::sidecar_desc_for_kind;

} // namespace

namespace {

using ArchiveValidationResult = std::variant<EntryMap, PackError>;

ArchiveValidationResult
validate_archive_entries(const std::filesystem::path &source) {
  const auto archive_bytes =
      platform::read_durable_file(source, max_pack_bytes);
  if (!archive_bytes.has_value())
    return PackError{"pack could not be read or exceeds " +
                     detail::size_limit_text(max_pack_bytes)};

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
    return PackError{"pack must contain between 1 and " +
                     std::to_string(max_pack_entries) + " entries"};

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
    const auto folded = core::try_casefold_utf8(path);
    if (!folded.has_value() || !folded_paths.insert(*folded).second)
      return PackError{"duplicate or case-colliding pack entry: " + path};
    if (stat.m_uncomp_size > max_pack_bytes ||
        total_uncompressed > max_pack_bytes - stat.m_uncomp_size)
      return PackError{"pack expands beyond " +
                       detail::size_limit_text(max_pack_bytes)};
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

// The declared entry exists with exactly the declared size and SHA-256.
const std::vector<std::byte> *verified_entry(const EntryMap &entries,
                                             const std::string_view path,
                                             const std::uint64_t size,
                                             const std::string_view hash) {
  const auto found = entries.find(path);
  if (found == entries.end() || found->second.size() != size ||
      core::sha256(found->second) != hash)
    return nullptr;
  return &found->second;
}

// Read-side mirror of detail::append_source_fields for base and module
// descriptors.
struct SourceDocument {
  std::string text;
  json::Value root;
  std::uint32_t schema{};
};

enum class SourceError { fields, entry, json, schema };
using SourceResult = std::variant<SourceDocument, SourceError>;

SourceResult read_source_descriptor(
    const json::Value &value, const std::string &path, const EntryMap &entries,
    std::set<std::string, std::less<>> &declared_paths, json::Reader &fields,
    const std::uint32_t max_schema, const std::size_t max_bytes,
    const bool module) {
  if (!fields.exact_fields(value,
                           module ? std::initializer_list<std::string_view>{
                                        "kind", "path", "schemaVersion", "size", "sha256"}
                                  : std::initializer_list<std::string_view>{
                                        "path", "schemaVersion", "size", "sha256"},
                           path))
    return SourceError::fields;
  const auto name = fields.string(value, "path", path, json::Required::yes,
                                  json::EmptyString::allow);
  const auto schema = fields.unsigned_integer(value, "schemaVersion", path,
                                               UINT64_MAX, json::Required::yes);
  const auto size = fields.unsigned_integer(value, "size", path, UINT64_MAX,
                                             json::Required::yes);
  const auto hash = fields.string(value, "sha256", path, json::Required::yes,
                                  json::EmptyString::allow);
  if (!name || !schema || *schema < 1U || *schema > max_schema || !size ||
      *size > max_bytes || !hash || !core::is_lowercase_sha256(*hash))
    return SourceError::fields;
  const auto *entry = verified_entry(entries, *name, *size, *hash);
  if (entry == nullptr || !declared_paths.insert(*name).second)
    return SourceError::entry;
  auto text = as_text(*entry);
  auto parsed = json::parse(text);
  auto *root = std::get_if<json::Value>(&parsed);
  if (!root) return SourceError::json;
  if (fields.unsigned_integer(*root, "schemaVersion", path + ".document",
                              UINT32_MAX, json::Required::yes) != *schema)
    return SourceError::schema;
  return SourceDocument{std::move(text), std::move(*root),
                        static_cast<std::uint32_t>(*schema)};
}

PackValidationResult validate_profile_descriptors(
    const core::json::Value &profile_values, const EntryMap &entries,
    ValidatedPack pack, std::set<std::string, std::less<>> declared_paths) {
  json::Reader fields;
  const auto invalid = [&](std::string message) {
    if (!fields.ok()) message += ": " + fields.error().describe();
    return pack_error<PackValidationResult>(std::move(message));
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
    auto base_source = read_source_descriptor(
        *base, descriptor_path + ".base", entries, declared_paths, fields,
        4U, core::max_profile_json_bytes, false);
    if (const auto *error = std::get_if<SourceError>(&base_source)) {
      switch (*error) {
      case SourceError::fields: return invalid("base profile descriptor fields are invalid");
      case SourceError::entry: return invalid("base profile size, hash, or path mismatch");
      case SourceError::json: return invalid("base profile JSON is invalid");
      case SourceError::schema: return invalid("base profile schema does not match manifest");
      }
    }
    auto &base_source_value = std::get<SourceDocument>(base_source);
    const auto parsed_profile = core::parse_profile_value(base_source_value.root);
    const auto *profile = std::get_if<core::GameProfile>(&parsed_profile);
    if (profile == nullptr || profile->id != *id)
      return invalid(
          "base profile is invalid or has a mismatched ID");

    PackProfile item;
    item.suggested_filename = *suggested;
    item.base_json = base_source_value.text;
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
      const auto module_kind = fields.string(module, "kind", module_descriptor_path,
                                             json::Required::yes, json::EmptyString::allow);
      if (!module_kind || !module_kinds.insert(*module_kind).second)
        return invalid("unsupported or invalid module descriptor");
      const auto desc = sidecar_desc_for_kind(*module_kind);
      if (!desc) return invalid("unsupported or invalid module descriptor");
      auto module_source = read_source_descriptor(
          module, module_descriptor_path, entries, declared_paths, fields,
          desc->max_schema, desc->max_bytes, true);
      if (const auto *error = std::get_if<SourceError>(&module_source)) {
        switch (*error) {
        case SourceError::fields: return invalid("unsupported or invalid module descriptor");
        case SourceError::entry: return invalid("module size, hash, or path mismatch");
        case SourceError::json: return invalid("module JSON is invalid");
        case SourceError::schema: return invalid("module schema does not match manifest");
        }
      }
      auto &module_source_value = std::get<SourceDocument>(module_source);
      const bool parsed_sidecar = detail::visit_sidecar(
          item, *desc, [&](auto &payload, const auto &traits) {
            using Traits = std::remove_cvref_t<decltype(traits)>;
            using SidecarProfile = typename Traits::Profile;
            auto parsed = Traits::parse(module_source_value.root, *profile);
            const auto *sidecar = std::get_if<SidecarProfile>(&parsed);
            if (sidecar == nullptr) return false;
            payload = ModulePayload<SidecarProfile>{.json = module_source_value.text,
                                                    .profile = *sidecar};
            return true;
          });
      if (!parsed_sidecar)
        return invalid(std::string(desc->kind) + " module is invalid");
      packed_schemas[std::string(*module_kind)] =
          module_source_value.schema;
    }
    for (const auto &desc : kSidecars) {
      if (base_source_value.schema < desc.minimum_profile_schema) continue;
      const auto declared = std::ranges::find(profile->modules, desc.module_kind,
                                               &core::ProfileModule::kind);
      const bool declares = declared != profile->modules.end();
      const auto packed = packed_schemas.find(std::string(desc.kind));
      const bool has_packed = packed != packed_schemas.end();
      if (declares != has_packed ||
          (declares && declared->schema_version != packed->second))
        return pack_error<PackValidationResult>(std::string("base ") +
                                  std::string(desc.kind) +
                                  " metadata disagrees with pack modules");
    }
    std::set<std::string, std::less<>> declared_references;
    for (const auto &reference : references->as_array()) {
      if (!reference.is_string() ||
          !core::is_lowercase_sha256(reference.as_string()) ||
          !declared_references.insert(reference.as_string()).second ||
          !pack.assets.contains(reference.as_string()))
        return pack_error<PackValidationResult>(
            "profile contains an invalid asset reference");
      item.asset_sha256.push_back(reference.as_string());
    }
    const auto actual_references = item.visual.has_value()
        ? detail::VisualSidecarTraits::assets(item.visual->profile)
        : std::vector<std::string>{};
    if (actual_references != item.asset_sha256)
      return pack_error<PackValidationResult>(
          "manifest assets do not match visual references");
    pack.profiles.push_back(std::move(item));
  }

  if (declared_paths.size() != entries.size())
    return pack_error<PackValidationResult>("pack contains undeclared files");
  return pack;
}

} // namespace

PackValidationResult
validate_profile_pack(const std::filesystem::path &source) {
  json::Reader fields;
  const auto invalid = [&](std::string message) {
    if (!fields.ok()) message += ": " + fields.error().describe();
    return pack_error<PackValidationResult>(std::move(message));
  };
  auto archive = validate_archive_entries(source);
  if (const auto *error = std::get_if<PackError>(&archive)) return *error;
  auto entries = std::move(std::get<EntryMap>(archive));

  const auto manifest_entry = entries.find("manifest.json");
  if (manifest_entry == entries.end() ||
      manifest_entry->second.size() > core::max_profile_json_bytes)
    return invalid("manifest.json is missing or exceeds " +
                   detail::size_limit_text(core::max_profile_json_bytes));
  const auto manifest_source = as_text(manifest_entry->second);
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
    return invalid("manifest must declare between 1 and " +
                   std::to_string(max_pack_profiles) + " profiles");

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
    const auto *entry = verified_entry(entries, *path, *size, *hash);
    if (entry == nullptr)
      return invalid("asset size or SHA-256 mismatch: " + *hash);
    if (!declared_paths.insert(*path).second ||
        !pack.assets.emplace(*hash, PackAsset{.sha256 = *hash, .bytes = *entry})
             .second)
      return invalid("duplicate asset declaration: " + *hash);
  }

  return validate_profile_descriptors(*profile_values, entries, std::move(pack),
                                      std::move(declared_paths));
}

} // namespace layerc::bundle
