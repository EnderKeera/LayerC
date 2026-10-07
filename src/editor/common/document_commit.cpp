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
                                    const ConfirmUpgrade &confirm_upgrade) {
  if (normalize_profile_bundle(bundle.profile, bundle.visual,
                               bundle.automation)
          .has_value())
    return {CommitResult::invalid_profile, std::nullopt};
  if (bundle.unreadable_module)
    return {CommitResult::blocked_by_sidecar, std::nullopt};
  if (bundle.upgrade_needed &&
      (!options.prompt_upgrade || !confirm_upgrade || !confirm_upgrade()))
    return {CommitResult::upgrade_declined, std::nullopt};
  const auto write =
      path.empty()
          ? layerc::platform::ProfileWriteResult{
                layerc::platform::ProfileWriteError::invalid_input}
          : layerc::platform::write_profile_bundle(
                path, bundle.profile, bundle.visual, bundle.automation,
                layerc::platform::KeepModule{});
  if (!write)
    return {CommitResult::write_failed, write.failure};
  return {};
}

CommitOutcome commit_document(ProfileDocument &document,
                              const std::filesystem::path &path,
                              const CommitOptions &options,
                              const ConfirmUpgrade &confirm_upgrade) {
  auto bundle = document.bundle();
  const auto outcome =
      commit_profile_bundle(path, bundle, options, confirm_upgrade);
  if (outcome.result != CommitResult::invalid_profile) {
    document.profile() = std::move(bundle.profile);
    document.visual() = std::move(bundle.visual);
    document.automation() = std::move(bundle.automation);
  }
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
