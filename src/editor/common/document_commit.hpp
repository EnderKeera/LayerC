#pragma once

#include "profile_document.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_transaction.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace layerc::editor {

// One commit attempt: why it stopped, plus the store failure behind
// write_failed so each frontend can word it without reaching into the store.
struct CommitOutcome {
  CommitResult result{CommitResult::ok};
  std::optional<layerc::platform::ProfileWriteError> write_error;

  [[nodiscard]] bool ok() const noexcept { return result == CommitResult::ok; }
};

// Asked at most once per commit, only when a legacy schema needs the one-way
// upgrade; false declines the commit.
using ConfirmUpgrade = std::function<bool()>;

// Validates the base profile and keeps the sidecar profile ids aligned.
[[nodiscard]] std::optional<layerc::core::ProfileError> normalize_profile_bundle(
    const layerc::core::GameProfile &profile,
    std::optional<layerc::core::VisualProfile> &visual,
    std::optional<layerc::core::AutomationProfile> &automation);

// The shared commit pipeline for every frontend: normalize, refuse to
// discard an unreadable module, obtain upgrade consent, then publish the base
// profile with its visual and automation intents through the durable store
// transaction. The companion sidecar is kept by default; a duplicate can
// supply a validated replacement without dropping the read-only module.
[[nodiscard]] CommitOutcome
commit_profile_bundle(const std::filesystem::path &path, ProfileBundle &bundle,
                      const CommitOptions &options,
                      const ConfirmUpgrade &confirm_upgrade,
                      layerc::platform::ModuleIntent<layerc::core::CompanionProfile>
                          companion = layerc::platform::KeepModule{},
                      const layerc::platform::ProfileMutationPlanner &additional_mutations = {});

// Commits a detached copy of the document's current values (the caller syncs
// its controls first). Only a successful durable commit installs normalized
// values; refusal or failure preserves both values and their storage. The
// document is marked saved only by the caller once the commit is accepted.
// An optional candidate mutates the detached bundle before validation, so a
// frontend commits a computed change (calibration, enablement) without touching
// the document first. A successful candidate commit is one undoable step; a
// refusal or failure leaves values, storage, path, history and dirty untouched.
[[nodiscard]] CommitOutcome
commit_document(ProfileDocument &document, const std::filesystem::path &path,
                const CommitOptions &options,
                const ConfirmUpgrade &confirm_upgrade,
                layerc::platform::ModuleIntent<layerc::core::CompanionProfile>
                    companion = layerc::platform::KeepModule{},
                const layerc::platform::ProfileMutationPlanner &additional_mutations = {},
                const std::function<void(ProfileBundle &)> &candidate = {});

// Same store file as the open document (canonical, case-insensitive where
// the store is).
[[nodiscard]] bool is_document_path(const ProfileDocument &document,
                                    const std::filesystem::path &path);

// A new profile never lands on another id's file just because both ids
// sanitize to the same name; an unreadable directory keeps the plain name.
[[nodiscard]] std::filesystem::path
default_profile_path(const std::filesystem::path &directory,
                     const layerc::core::GameProfile &profile);

// "Profile write failed: <reason>" for a write_failed outcome.
[[nodiscard]] std::string
write_failure_text(std::optional<layerc::platform::ProfileWriteError> error);

} // namespace layerc::editor
