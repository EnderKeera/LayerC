// Profile-pack import: advisory previews share the planner with publication,
// which rebuilds the plan only after acquiring recovered writer ownership.
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
#include <tuple>
#include <type_traits>
#include <utility>

namespace layerc::bundle {
namespace {

using detail::asset_path_for_profile;
using detail::kSidecars;
using detail::pack_error;

} // namespace

std::optional<std::filesystem::path>
resolve_suggested_profile_filename(const std::string_view suggested_filename) {
  auto filename = core::try_utf8_to_wide(suggested_filename);
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
  const auto sanitized = core::try_wide_to_utf8(*filename);
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

// Full packs replace the complete bundle. A base-only JSON import instead
// keeps missing modules, including unsupported and unreadable module evidence.
template <typename Profile>
platform::ModuleIntent<Profile>
module_intent(const std::optional<ModulePayload<Profile>> &slot,
              const bool keep_missing) {
  if (slot.has_value()) return slot->profile;
  if (keep_missing) return platform::KeepModule{};
  return platform::RemoveModule{};
}

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
  return core::try_casefold_utf8(
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

  std::vector<const ImportChoice *> ordered_choices;
  std::set<std::size_t> selected_indices;
  for (const auto &choice : choices) {
    if (choice.profile_index >= pack.profiles.size() ||
        !selected_indices.insert(choice.profile_index).second)
      return pack_error<ImportPlanResult>("import choices are invalid or duplicated");
    if (choice.action != ImportAction::skip &&
        choice.action != ImportAction::copy &&
        choice.action != ImportAction::replace)
      return pack_error<ImportPlanResult>("import action is invalid");
    ordered_choices.push_back(&choice);
  }
  // Resolve original destinations before copies, then allocate copies in pack
  // order, not UI choice order. Both IDs and filenames are order-independent.
  std::ranges::sort(ordered_choices, [](const auto *left, const auto *right) {
    const bool left_copy = left->action == ImportAction::copy;
    const bool right_copy = right->action == ImportAction::copy;
    if (left_copy != right_copy) return !left_copy;
    return left->profile_index < right->profile_index;
  });

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

  // An incoming original identity is reserved even when it is not in the
  // store yet. Copy(foo) must never consume Replace(foo-copy)'s identity.
  for (const auto &choice : choices)
    if (choice.action == ImportAction::replace)
      used_ids.insert(pack.profiles[choice.profile_index].profile.id);

  std::vector<ResolvedImportEntry> resolved_entries(choices.size());
  std::set<std::string, std::less<>> selected_targets;
  std::vector<platform::ProfileFileMutation> mutations;
  std::map<std::filesystem::path, std::string> required_assets;

  for (const auto *ordered_choice : ordered_choices) {
    const auto &choice = *ordered_choice;
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
    const auto suggested_folded = core::try_casefold_utf8(
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
    // Return entries in the caller's order so advisory choices remain bound
    // to their own destinations even though allocation is canonical.
    resolved_entries[static_cast<std::size_t>(ordered_choice - choices.data())] =
        {.profile_index = choice.profile_index,
         .id = imported.profile.id,
         .target = target,
         .collides = collides,
         .skipped = skipped};
    if (skipped) continue;
    const auto target_folded = folded_filename(target);
    if (!target_folded.has_value() ||
        !selected_targets.insert(*target_folded).second)
      return pack_error<ImportPlanResult>(
          "multiple selected profiles resolve to the same output file");
    used_filenames.insert(*target_folded);
    used_ids.insert(imported.profile.id);

    for (const auto &desc : kSidecars) {
      detail::visit_sidecar(
          imported, desc, [&](const auto &module, const auto &traits) {
            using Traits = std::remove_cvref_t<decltype(traits)>;
            if (module.has_value())
              for (const auto &hash : Traits::assets(module->profile))
                required_assets.emplace(asset_path_for_profile(target, hash),
                                        hash);
          });
    }
    // The store's save policy owns base metadata, recovery copies, markers,
    // and tombstones; the batch adds only content-addressed assets.
    auto intents = std::apply(
        [&](const auto &...slots) {
          return platform::ProfileModuleIntents{
              module_intent(slots, choice.keep_missing_modules)...};
        },
        imported.modules);
    const auto planned = platform::plan_profile_bundle_mutations(
        target, imported.profile, std::move(intents),
        choice.require_absent || choice.action == ImportAction::copy ||
            !collides,
        mutations);
    if (const auto failure = planned.failure)
      return pack_error<ImportPlanResult>(
          std::string("selected profile could not be planned: ") +
          platform::profile_write_error_text(*failure));
  }


  // Validate the complete winning set, not just the allocator's reservations:
  // replaced files surrender their old identities; untouched and skipped
  // files retain theirs. Every final identity and target must be unique.
  std::set<std::string, std::less<>> final_ids;
  std::set<std::string, std::less<>> final_targets;
  const auto add_final = [&](const std::string &id,
                             const std::filesystem::path &target) {
    const auto folded = folded_filename(target);
    return folded.has_value() && final_ids.insert(id).second &&
           final_targets.insert(*folded).second;
  };
  for (const auto &stored : existing) {
    const auto folded = folded_filename(stored.path);
    if (folded.has_value() && selected_targets.contains(*folded)) continue;
    if (!add_final(stored.id, stored.path))
      return pack_error<ImportPlanResult>("final profile IDs or destinations are duplicated");
  }
  for (const auto &entry : resolved_entries)
    if (!entry.skipped && !add_final(entry.id, entry.target))
      return pack_error<ImportPlanResult>("final profile IDs or destinations are duplicated");

  return ImportPlan{.mutations = std::move(mutations),
                    .required_assets = std::move(required_assets),
                    .entries = std::move(resolved_entries)};
}

} // namespace

detail::ImportCommitResult detail::commit_profile_import(
    const ValidatedPack &pack, const std::span<const ImportChoice> choices,
    const std::filesystem::path &profile_directory) {
  std::optional<PackError> planning_error;
  bool published{};
  const auto committed = platform::commit_profile_transaction(
      profile_directory, [&](std::vector<platform::ProfileFileMutation> &mutations)
                             -> platform::ProfileWriteResult {
        const auto reject = [&](std::string message) {
          planning_error = PackError{std::move(message)};
          return platform::ProfileWriteResult{
              platform::ProfileWriteError::invalid_input};
        };
        auto built = build_import_plan(pack, choices, profile_directory);
        if (const auto *error = std::get_if<PackError>(&built))
          return reject(error->message);
        auto plan = std::move(std::get<ImportPlan>(built));
        // Asset inspection, backups, and document planning all observe the
        // recovered store under the same owner as publication.
        for (const auto &[target, hash] : plan.required_assets) {
          const auto found = pack.assets.find(hash);
          if (found == pack.assets.end())
            return reject("selected profile asset is missing");
          if (target.empty()) return reject("asset destination is invalid");
          const auto existing_bytes =
              platform::read_durable_file(target, max_pack_asset_bytes);
          if (existing_bytes.has_value()) {
            if (core::sha256(*existing_bytes) != hash)
              return reject("existing content-addressed asset is corrupt: " + hash);
            continue;
          }
          if (existing_bytes.status != platform::FileReadStatus::missing)
            return reject("asset destination could not be read");
          mutations.push_back({.target = target, .contents = found->second.bytes});
        }
        mutations.insert(mutations.end(),
                         std::make_move_iterator(plan.mutations.begin()),
                         std::make_move_iterator(plan.mutations.end()));
        published = !mutations.empty();
        return {};
      });
  if (planning_error) return *planning_error;
  if (const auto error = committed.failure)
    return pack_error<ImportCommitResult>(
        std::string("profile-pack transaction failed: ") +
        platform::profile_write_error_text(*error));
  return published;
}

PackOperationResult
import_profile_pack(const ValidatedPack &pack,
                    const std::span<const ImportChoice> choices,
                    const std::filesystem::path &profile_directory) {
  const auto committed = detail::commit_profile_import(pack, choices, profile_directory);
  if (const auto *error = std::get_if<PackError>(&committed)) return *error;
  // LayerD may not be running. Signal only after a nonempty durable commit;
  // tests exercise the internal publication boundary without any broadcast.
  if (std::get<bool>(committed)) (void)platform::signal_profile_reload();
  return std::monostate{};
}

PackOperationResult recover_profile_pack_transactions(
    const std::filesystem::path &profile_directory) {
  const auto recovered = platform::recover_profile_transactions(profile_directory);
  if (const auto error = recovered.failure)
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
