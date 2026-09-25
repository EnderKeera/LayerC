// Profile-pack import: one side-effect-free plan shared by preview, dry-run
// validation, and the committing transaction.
#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"
#include "pack_detail.hpp"
#include "pack_sidecar_table.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <type_traits>
#include <utility>

namespace layerc::bundle {
namespace {

using detail::asset_path_for_profile;
using detail::kSidecars;
using detail::owned_bytes;
using detail::pack_error;

// Path <-> UTF-8 conversions live in platform/text.hpp (path_to_utf8 /
// path_from_utf8) — this TU uses the canonical pair so failure modes match
// the transaction engine and store scan.
platform::ProfileWriteResult commit_pack_transaction(
    const std::filesystem::path &profile_directory,
    const std::span<const platform::ProfileFileMutation> mutations) {
  const auto roots = platform::profile_transaction_scope(profile_directory);
  if (!roots.has_value()) return {platform::ProfileWriteError::scope_rejected};
  return platform::commit_profile_transaction(*roots, mutations);
}

platform::ProfileWriteResult recover_pack_transaction(
    const std::filesystem::path &profile_directory) {
  const auto roots = platform::profile_transaction_scope(profile_directory);
  if (!roots.has_value()) return {platform::ProfileWriteError::scope_rejected};
  return platform::recover_profile_transactions(*roots);
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
  if (resolved.empty()) return std::nullopt;
  return resolved;
}

namespace {

struct ImportPlan {
  std::vector<platform::ProfileFileMutation> mutations;
  std::map<std::filesystem::path, std::string> required_assets;
  std::vector<ResolvedImportEntry> entries;
};

using ImportPlanResult = std::variant<ImportPlan, PackError>;

// Copy imports get a fresh profile id and matching sidecar profileIds so the
// copied bundle stays internally consistent with the new base identity.
void retarget_sidecar_profile_ids(PackProfile &imported) {
  for (const auto &desc : kSidecars) {
    detail::visit_sidecar(imported, desc, [&](auto &module, const auto &) {
      if (module.has_value())
        module->profile.profile_id = imported.profile.id;
    });
  }
}

std::string unique_copy_profile_id(
    const std::string &original_id,
    const std::set<std::string, std::less<>> &used_ids) {
  const auto stem = original_id + "-copy";
  auto candidate = stem;
  for (std::uint32_t suffix = 2; used_ids.contains(candidate); ++suffix)
    candidate = stem + "-" + std::to_string(suffix);
  return candidate;
}

// Case-folded UTF-8 filename used for collision checks.
std::optional<std::string>
folded_filename(const std::filesystem::path &path) {
  return platform::try_casefold_path(
      platform::path_to_utf8(path.filename()).value_or(std::string{}));
}

std::optional<std::filesystem::path> resolve_copy_target(
    const std::filesystem::path &profile_directory,
    const std::filesystem::path &suggested,
    const std::set<std::string, std::less<>> &used_filenames) {
  const auto stem =
      platform::path_to_utf8(suggested.stem()).value_or(std::string{}) +
      " copy";
  const auto copy_path = platform::path_from_utf8(stem + ".json");
  if (!copy_path.has_value()) return std::nullopt;
  auto target = profile_directory / *copy_path;
  for (std::uint32_t suffix = 2;; ++suffix) {
    const auto folded = folded_filename(target);
    if (folded.has_value() && !used_filenames.contains(*folded))
      return target;
    const auto suffixed =
        platform::path_from_utf8(stem + " " + std::to_string(suffix) + ".json");
    if (!suffixed.has_value()) return std::nullopt;
    target = profile_directory / *suffixed;
  }
}

ImportPlanResult build_import_plan(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  if (choices.empty() || choices.size() > pack.profiles.size())
    return pack_error<ImportPlanResult>("select at least one valid pack profile");

  struct ExistingProfile {
    std::filesystem::path path;
    std::string id;
  };
  std::vector<ExistingProfile> existing;
  std::set<std::string, std::less<>> used_ids;
  std::set<std::string, std::less<>> used_filenames;
  const auto scan = platform::scan_profile_directory(profile_directory);
  if (!scan.error.empty()) return pack_error<ImportPlanResult>(scan.error);
  for (const auto &path : scan.base_paths) {
    const auto folded = folded_filename(path);
    if (!folded.has_value() || !used_filenames.insert(*folded).second)
      return pack_error<ImportPlanResult>("profile directory contains case-colliding filenames");
  }
  for (const auto &stored : scan.profiles) {
    if (!used_ids.insert(stored.profile.id).second)
      return pack_error<ImportPlanResult>("profile directory contains duplicate profile IDs");
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
      return pack_error<ImportPlanResult>("import choices are invalid or duplicated");
    if (choice.action != ImportAction::skip &&
        choice.action != ImportAction::copy &&
        choice.action != ImportAction::replace)
      return pack_error<ImportPlanResult>("import action is invalid");
    auto imported = pack.profiles[choice.profile_index];
    const auto resolved_suggested =
        resolve_suggested_profile_filename(imported.suggested_filename);
    if (!resolved_suggested.has_value())
      return pack_error<ImportPlanResult>("suggested profile filename is invalid");
    const auto suggested = *resolved_suggested;

    const auto by_id =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          return candidate.id == imported.profile.id;
        });
    const auto suggested_folded = platform::try_casefold_path(
        platform::path_to_utf8(suggested).value_or(std::string{}));
    if (!suggested_folded.has_value())
      return pack_error<ImportPlanResult>("suggested profile filename is invalid");
    const auto by_filename =
        std::ranges::find_if(existing, [&](const ExistingProfile &candidate) {
          const auto folded = folded_filename(candidate.path);
          return folded.has_value() && *folded == *suggested_folded;
        });

    std::filesystem::path target;
    if (choice.action == ImportAction::skip) {
      if (by_id != existing.end())
        target = by_id->path;
      else if (by_filename != existing.end())
        target = by_filename->path;
      else {
        const auto matching_path = std::ranges::find_if(
            scan.base_paths, [&](const std::filesystem::path &candidate) {
              return folded_filename(candidate) == suggested_folded;
            });
        target = matching_path != scan.base_paths.end()
                     ? *matching_path : profile_directory / suggested;
      }
    } else if (choice.action == ImportAction::copy) {
      imported.profile.id =
          unique_copy_profile_id(imported.profile.id, used_ids);
      retarget_sidecar_profile_ids(imported);
      const auto copy_target =
          resolve_copy_target(profile_directory, suggested, used_filenames);
      if (!copy_target.has_value())
        return pack_error<ImportPlanResult>("suggested filename is not valid UTF-8");
      target = *copy_target;
    } else {
      if (by_id != existing.end() && by_filename != existing.end() &&
          by_id->path != by_filename->path)
        return pack_error<ImportPlanResult>("profile ID and filename conflict with different files; "
                       "import a copy instead");
      if (by_id != existing.end())
        target = by_id->path;
      else if (by_filename != existing.end())
        target = by_filename->path;
      else if (used_filenames.contains(*suggested_folded))
        return pack_error<ImportPlanResult>("suggested filename belongs to an unreadable or invalid "
                       "profile; import a copy instead");
      else
        target = profile_directory / suggested;
    }
    if (choice.expected_target.has_value() &&
        choice.expected_target->lexically_normal() != target.lexically_normal())
      return pack_error<ImportPlanResult>("import destination changed; review the import again");
    const bool collides = by_id != existing.end() ||
                          used_filenames.contains(*suggested_folded);
    const bool skipped = choice.action == ImportAction::skip;
    resolved_entries.push_back({.profile_index = choice.profile_index,
                                .id = imported.profile.id,
                                .target = target,
                                .collides = collides,
                                .skipped = skipped});
    if (skipped) continue;
    const auto target_folded = folded_filename(target);
    if (!target_folded.has_value() ||
        !selected_targets.insert(*target_folded).second)
      return pack_error<ImportPlanResult>(
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
      return pack_error<ImportPlanResult>("existing profile could not be protected by a backup");
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
            if (module.has_value())
              for (const auto &hash : Traits::assets(module->profile))
                required_assets.emplace(asset_path_for_profile(target, hash),
                                        hash);
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
      return pack_error<PackOperationResult>("selected profile asset is missing");
    if (target.empty()) return pack_error<PackOperationResult>("asset destination is invalid");
    const auto existing_bytes =
        platform::read_durable_file(target, max_pack_asset_bytes);
    if (existing_bytes.has_value()) {
      if (core::sha256(*existing_bytes) != hash)
        return pack_error<PackOperationResult>("existing content-addressed asset is corrupt: " + hash);
      continue;
    }
    if (existing_bytes.status != platform::FileReadStatus::missing)
      return pack_error<PackOperationResult>("asset destination could not be read");
    asset_mutations.push_back(
        {.target = target, .contents = found->second.bytes});
  }
  asset_mutations.insert(asset_mutations.end(),
                         std::make_move_iterator(plan.mutations.begin()),
                         std::make_move_iterator(plan.mutations.end()));
  if (asset_mutations.empty()) return std::monostate{};
  const auto committed = commit_pack_transaction(profile_directory, asset_mutations);
  if (const auto error = committed.error())
    return pack_error<PackOperationResult>(std::string("profile-pack transaction failed: ") +
                   platform::profile_write_error_text(*error));
  // LayerD may legitimately not be running. The committed files are the
  // source of truth and will be loaded at its next startup.
  (void)platform::signal_profile_reload();
  return std::monostate{};
}

PackOperationResult recover_profile_pack_transactions(
    const std::filesystem::path &profile_directory) {
  const auto recovered = recover_pack_transaction(profile_directory);
  if (const auto error = recovered.error())
    return pack_error<PackOperationResult>(std::string("profile-pack recovery failed: ") +
                   platform::profile_write_error_text(*error));
  return std::monostate{};
}

ImportPreview preview_profile_import(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  auto built = build_import_plan(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&built))
    return {.entries = {}, .choices = {}, .error = *error};
  auto entries = std::move(std::get<ImportPlan>(built).entries);
  std::vector<ImportChoice> bound(choices.begin(), choices.end());
  for (std::size_t index = 0; index < bound.size(); ++index) {
    bound[index].expected_target = entries[index].target;
    bound[index].require_absent = bound[index].action == ImportAction::copy ||
                                   !entries[index].collides;
  }
  return {.entries = std::move(entries),
          .choices = std::move(bound), .error = std::nullopt};
}

std::optional<PackError> validate_import_choices(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  auto built = build_import_plan(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&built)) return *error;
  return std::nullopt;
}


} // namespace layerc::bundle
