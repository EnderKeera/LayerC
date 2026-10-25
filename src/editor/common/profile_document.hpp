#pragma once

#include "editor_history.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace layerc::editor {

enum class DocumentChangeKind { text, immediate, drag };

enum class ModuleEditStatus { accepted, unchanged, blocked, refused, invalid };

struct ModuleEditOutcome {
  ModuleEditStatus status{ModuleEditStatus::unchanged};
  std::string message;
  [[nodiscard]] bool accepted() const noexcept {
    return status == ModuleEditStatus::accepted;
  }
};

struct CooldownRemovalError {
  std::string message;
};

// Shared panel normalization for adding and removing a cooldown.
void sync_bar_panels(
    layerc::core::GameProfile &profile,
    std::optional<std::string_view> added = std::nullopt);

// Why a shared document commit stopped. Call sites map this to their own
// status or dialog text so autosave, save, visual, and enablement keep
// distinct wording without duplicating the pipeline.
enum class CommitResult {
  ok,
  invalid_fields,
  invalid_profile,
  upgrade_declined,
  blocked_by_sidecar,
  write_failed,
};

// The one commit input: a base profile plus each edited module's write
// intent (present = replace, absent = remove; the companion module is not
// edited here and is always kept). An unreadable module has no parsed
// profile, so its absence is not a removal intent: it blocks the commit and
// its recovery bytes and backups survive.
struct ProfileBundle {
  layerc::core::GameProfile profile;
  std::optional<layerc::core::VisualProfile> visual;
  std::optional<layerc::core::AutomationProfile> automation;
  bool unreadable_module{};
  // A legacy schema that needs the one-way upgrade confirmation.
  bool upgrade_needed{};
};

[[nodiscard]] ProfileBundle
loaded_bundle(layerc::platform::ProfileBundleLoad loaded);

struct CommitOptions {
  // Prompt before a one-way legacy schema upgrade. When false, a required
  // upgrade fails the commit (autosave without explicit approval).
  bool prompt_upgrade = true;
  // After a document commit, reload the profile list instead of syncing the
  // current row only.
  bool refresh_list = false;
  // New drafts must not overwrite a file created after the list scan.
  // Checked inside the recovered transaction's writer ownership.
  bool create_only = false;
};

// Why a draft exists. A fresh draft is clean until its first accepted edit; a
// copy is unsaved by construction and stays dirty until committed.
enum class DraftOrigin {
  fresh,
  copy,
};

// How a successful commit enters history. Ordinary saves only rebase the saved
// values; a committed candidate change is one undoable step.
enum class CommitRecord {
  rebase,
  record_edit,
};

// Owns the base profile, editable sidecars, persistence state, and undo history.
// A document path and its module state move together; callers cannot retain a
// detached copy of one persistence flag outside this object.
class ProfileDocument final {
public:
  [[nodiscard]] const layerc::core::GameProfile &profile() const noexcept {
    return profile_;
  }

  [[nodiscard]] const std::optional<layerc::core::VisualProfile> &
  visual() const noexcept {
    return visual_.profile;
  }

  [[nodiscard]] const std::optional<layerc::core::AutomationProfile> &
  automation() const noexcept {
    return automation_.profile;
  }

  [[nodiscard]] layerc::platform::ModuleSidecarState
  visual_state() const noexcept {
    return visual_.state;
  }
  [[nodiscard]] const std::string &visual_error() const noexcept {
    return visual_.diagnostic;
  }

  [[nodiscard]] layerc::platform::ModuleSidecarState
  automation_state() const noexcept {
    return automation_.state;
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] bool legacy_base_schema() const noexcept {
    return legacy_base_schema_;
  }
  [[nodiscard]] bool legacy_visual_schema() const noexcept {
    return legacy_visual_schema_;
  }
  void acknowledge_upgrade() noexcept { upgrade_acknowledged_ = true; }
  [[nodiscard]] bool upgrade_acknowledged() const noexcept {
    return upgrade_acknowledged_;
  }
  // Persisted cleanliness is owned by the saved history, never a side flag.
  [[nodiscard]] bool dirty() const noexcept {
    return !history_.is_saved(profile_, visual_.profile, automation_.profile);
  }
  [[nodiscard]] const EditorProfileHistory &history() const noexcept {
    return history_;
  }

  // Deletion is conservative: preflight reports every group/rule dependency.
  // The accepted removal is one recorded history transaction.
  [[nodiscard]] std::optional<CooldownRemovalError>
  cooldown_removal_error(std::string_view id) const;
  [[nodiscard]] std::optional<CooldownRemovalError>
  remove_cooldown(std::string_view id);

  // An invalid module without a parsed profile (see ProfileBundle).
  [[nodiscard]] bool has_unreadable_module() const noexcept {
    return (!visual_.profile &&
            visual_.state == layerc::platform::ModuleSidecarState::invalid) ||
           (!automation_.profile &&
            automation_.state == layerc::platform::ModuleSidecarState::invalid);
  }
  // ---- F01 transitions: the document owns draft replacement, saved-path
  // transitions and snapshot application. Committed values replace the document
  // only through accept_commit, after the store accepted them.
  void open(std::filesystem::path path,
            layerc::platform::ProfileBundleLoad loaded);
  void start_draft(layerc::core::GameProfile profile, DraftOrigin origin,
                   std::optional<layerc::core::VisualProfile> visual = std::nullopt,
                   std::optional<layerc::core::AutomationProfile> automation =
                       std::nullopt);
  void close();
  void accept_commit(std::filesystem::path path, ProfileBundle committed,
                     CommitRecord record = CommitRecord::rebase);
  [[nodiscard]] bool can_undo() const noexcept { return history_.can_undo(); }
  [[nodiscard]] bool can_redo() const noexcept { return history_.can_redo(); }
  // Every visual profile current values or history can restore (asset retention).
  template <typename Visit>
  void for_each_retained_visual(Visit &&visit) const {
    history_.for_each_visual_profile(visit);
    visit(visual_.profile);
    if (retained_visual_) visit(retained_visual_->optional_profile());
  }
  // Recording variants: an accepted edit is exactly one history transaction.
  ModuleEditOutcome edit_profile(
      const std::function<void(layerc::core::GameProfile &)> &edit,
      DocumentChangeKind kind,
      const layerc::core::CompanionProfile *companion = nullptr);
  ModuleEditOutcome edit_visual(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::VisualProfile> &)> &edit,
      DocumentChangeKind kind);
  ModuleEditOutcome edit_automation(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::AutomationProfile> &)> &edit,
      DocumentChangeKind kind, bool repair = false);
  // The only edit allowed to replace an unreadable visual module.
  ModuleEditOutcome
  replace_visual_from_calibration(layerc::core::VisualProfile visual);
  bool undo();
  bool redo();
  void end_text_coalescing() noexcept { history_.end_text_coalescing(); }

  // The document as a commit input (the caller syncs controls first).
  [[nodiscard]] ProfileBundle bundle() const {
    return {.profile = profile_,
            .visual = visual_.profile,
            .automation = automation_.profile,
            .unreadable_module = has_unreadable_module(),
            .upgrade_needed = (legacy_base_schema_ || legacy_visual_schema_) &&
                              !upgrade_acknowledged_};
  }
private:
  // Value implementations of the recorded edits: they validate and replace a
  // candidate but never record history; the public overloads record once.
  [[nodiscard]] ModuleEditOutcome edit_profile_value(
      const std::function<void(layerc::core::GameProfile &)> &edit,
      const layerc::core::CompanionProfile *companion);
  [[nodiscard]] ModuleEditOutcome edit_visual_value(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::VisualProfile> &)> &edit);
  [[nodiscard]] ModuleEditOutcome edit_automation_value(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::AutomationProfile> &)> &edit,
      bool repair);

  // Sidecar value, state, and error move together so no caller can leave a
  // valid state beside a stale error or an invalid state beside a value.
  void set_visual(const layerc::platform::VisualSidecarLoad &loaded,
                  const bool legacy) {
    visual_ = decltype(visual_)::from(loaded);
    legacy_visual_schema_ = legacy;
  }
  void set_automation(const layerc::platform::AutomationSidecarLoad &loaded) {
    automation_ = decltype(automation_)::from(loaded);
  }
  void mark_visual_edited() { visual_.mark_edited(); }
  void mark_automation_edited() { automation_.mark_edited(); }

  layerc::core::GameProfile profile_;
  layerc::platform::ModuleSidecar<layerc::core::VisualProfile> visual_;
  layerc::platform::ModuleSidecar<layerc::core::AutomationProfile> automation_;
  std::filesystem::path path_;
  bool legacy_base_schema_{};
  bool legacy_visual_schema_{};
  bool upgrade_acknowledged_{};
  // The loaded invalid visual sidecar, so undo can restore exactly that state
  // and diagnostic after a calibration repair.
  std::optional<layerc::platform::VisualSidecarLoad> retained_visual_;
  EditorProfileHistory history_;
};

enum class OpenDocumentEnablement { commit_open_edits, require_clean };
enum class EnablementRefusal { dirty_open_document, unreadable, invalid_profile };

// Selects the open edits or stored bundle without committing or changing history.
[[nodiscard]] std::variant<ProfileBundle, EnablementRefusal>
profile_enablement_bundle(const ProfileDocument &document, bool is_current,
                          const std::filesystem::path &target,
                          OpenDocumentEnablement policy);

} // namespace layerc::editor
