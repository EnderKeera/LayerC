#include "document_commit.hpp"

#include "layerc/platform/profile_store.hpp"

#include <utility>

namespace layerc::editor {

std::optional<layerc::core::ProfileError> normalize_profile_bundle(
    const layerc::core::GameProfile &profile,
    std::optional<layerc::core::VisualProfile> &visual,
    std::optional<layerc::core::AutomationProfile> &automation) {
  if (auto error = layerc::core::validate_profile(profile)) return error;
  if (visual) visual->profile_id = profile.id;
  if (automation) automation->profile_id = profile.id;
  return std::nullopt;
}

CommitOutcome commit_profile_bundle(const std::filesystem::path &path,
                                    ProfileBundle &bundle,
                                    const CommitOptions &options,
                                    const ConfirmUpgrade &confirm_upgrade,
                                    layerc::platform::ModuleIntent<layerc::core::CompanionProfile>
                                        companion,
                                    const layerc::platform::ProfileMutationPlanner &additional_mutations) {
  if (normalize_profile_bundle(bundle.profile, bundle.visual,
                               bundle.automation)
          .has_value())
    return {CommitResult::invalid_profile, std::nullopt};
  if (bundle.unreadable_module)
    return {CommitResult::blocked_by_sidecar, std::nullopt};
  if (bundle.upgrade_needed &&
      (!options.prompt_upgrade || !confirm_upgrade || !confirm_upgrade()))
    return {CommitResult::upgrade_declined, std::nullopt};
  namespace platform = layerc::platform;
  const auto planner = [&](std::vector<platform::ProfileFileMutation> &mutations) {
    if (additional_mutations) {
      const auto extra = additional_mutations(mutations);
      if (!extra) return extra;
    }
    return platform::plan_profile_bundle_mutations(
        path, bundle.profile,
        platform::ProfileModuleIntents{bundle.visual, bundle.automation, companion},
        options.create_only, mutations);
  };
  const auto write = path.empty()
                         ? platform::ProfileWriteResult{platform::ProfileWriteError::invalid_input}
                         : platform::commit_profile_transaction(
                               platform::ProfileTransactionScope(path.parent_path()),
                               platform::ProfileMutationPlan(planner));
  if (!write)
    return {CommitResult::write_failed, write.failure};
  return {};
}

CommitOutcome commit_document(ProfileDocument &document,
                              const std::filesystem::path &path,
                              const CommitOptions &options,
                              const ConfirmUpgrade &confirm_upgrade,
                              layerc::platform::ModuleIntent<layerc::core::CompanionProfile>
                                  companion,
                              const layerc::platform::ProfileMutationPlanner &additional_mutations,
                              const std::function<void(ProfileBundle &)> &candidate) {
  auto bundle = document.bundle();
  if (candidate) {
    candidate(bundle);
    // A calibration repair supplies a visual, so it is no longer blocked by an
    // unreadable stored module; anything still missing keeps the block.
    bundle.unreadable_module =
        (document.visual_state() == layerc::platform::ModuleSidecarState::invalid &&
         !bundle.visual) ||
        (document.automation_state() == layerc::platform::ModuleSidecarState::invalid &&
         !bundle.automation);
    if (bundle.visual) {
      const auto error =
          layerc::core::validate_visual_profile(*bundle.visual, bundle.profile);
      if (!error.message.empty())
        return {CommitResult::invalid_profile, std::nullopt};
    }
  }
  const auto outcome =
      commit_profile_bundle(path, bundle, options, confirm_upgrade, std::move(companion),
                            additional_mutations);
  // Only a successful durable commit installs values and the saved transition.
  if (outcome.ok())
    document.accept_commit(path, std::move(bundle),
                           candidate ? CommitRecord::record_edit
                                     : CommitRecord::rebase);
  return outcome;
}

bool is_document_path(const ProfileDocument &document,
                      const std::filesystem::path &path) {
  return !document.path().empty() &&
         layerc::platform::canonical_profile_key(path) ==
             layerc::platform::canonical_profile_key(document.path());
}

std::filesystem::path
default_profile_path(const std::filesystem::path &directory,
                     const layerc::core::GameProfile &profile) {
  const auto scan = layerc::platform::scan_profile_directory(directory);
  if (!scan.error.empty())
    return directory / layerc::platform::profile_store_filename(profile.id);
  return scan.path_for(directory, profile.id);
}

std::string
write_failure_text(const std::optional<layerc::platform::ProfileWriteError> error) {
  return std::string("Profile write failed: ") +
         layerc::platform::profile_write_error_text(
             error.value_or(layerc::platform::ProfileWriteError::io_failed));
}

} // namespace layerc::editor
