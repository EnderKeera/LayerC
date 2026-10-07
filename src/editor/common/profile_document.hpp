#pragma once

#include "editor_history.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace layerc::editor {

enum class DocumentChangeKind { text, immediate, drag };

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
};

// Owns the base profile, editable sidecars, persistence state, and undo history.
// A document path and its module state move together; callers cannot retain a
// detached copy of one persistence flag outside this object.
class ProfileDocument final {
public:
  [[nodiscard]] layerc::core::GameProfile &profile() noexcept { return profile_; }
  [[nodiscard]] const layerc::core::GameProfile &profile() const noexcept {
    return profile_;
  }

  [[nodiscard]] std::optional<layerc::core::VisualProfile> &visual() noexcept {
    return visual_.profile;
  }
  [[nodiscard]] const std::optional<layerc::core::VisualProfile> &
  visual() const noexcept {
    return visual_.profile;
  }

  [[nodiscard]] std::optional<layerc::core::AutomationProfile> &
  automation() noexcept {
    return automation_.profile;
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

  [[nodiscard]] std::filesystem::path &path() noexcept { return path_; }
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
  void set_dirty(const bool dirty) noexcept { dirty_ = dirty; }
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }
  [[nodiscard]] EditorProfileHistory &history() noexcept { return history_; }
  // A freshly opened or created document: its values are the only history.
  void reset_history() {
    history_.reset(profile_, visual_.profile, automation_.profile);
  }
  // After the document's own path was committed: clean, written in the
  // current schemas, and the saved values become the history baseline.
  void mark_saved() {
    dirty_ = false;
    clear_legacy_schemas();
    history_.rebase(profile_, visual_.profile, automation_.profile);
  }
  [[nodiscard]] const EditorProfileHistory &history() const noexcept {
    return history_;
  }

  // Deletion is conservative: preflight reports every group/rule dependency.
  // The operation validates a complete candidate before replacing any value;
  // its caller records the accepted edit as one normal history transaction.
  [[nodiscard]] std::optional<CooldownRemovalError>
  cooldown_removal_error(std::string_view id) const;
  [[nodiscard]] std::optional<CooldownRemovalError>
  remove_cooldown(std::string_view id);

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
  // An invalid module without a parsed profile (see ProfileBundle).
  [[nodiscard]] bool has_unreadable_module() const noexcept {
    return (!visual_.profile &&
            visual_.state == layerc::platform::ModuleSidecarState::invalid) ||
           (!automation_.profile &&
            automation_.state == layerc::platform::ModuleSidecarState::invalid);
  }
  // The document as a commit input (the caller syncs controls first).
  [[nodiscard]] ProfileBundle bundle() const {
    return {.profile = profile_,
            .visual = visual_.profile,
            .automation = automation_.profile,
            .unreadable_module = has_unreadable_module(),
            .upgrade_needed = (legacy_base_schema_ || legacy_visual_schema_) &&
                              !upgrade_acknowledged_};
  }
  void mark_visual_edited() { visual_.mark_edited(); }
  void mark_automation_edited() { automation_.mark_edited(); }
  void clear_sidecars() {
    set_visual({}, false);
    set_automation({});
  }
  // The document was rewritten in the current schemas.
  void clear_legacy_schemas() noexcept {
    legacy_base_schema_ = false;
    legacy_visual_schema_ = false;
    upgrade_acknowledged_ = false;
  }
  void assign_bundle(std::filesystem::path path,
                     layerc::platform::ProfileBundleLoad bundle) {
    profile_ = std::move(bundle.profile);
    path_ = std::move(path);
    set_visual(bundle.modules.get<layerc::core::VisualModule>(),
               bundle.legacy_visual_schema);
    set_automation(bundle.modules.get<layerc::core::AutomationModule>());
    legacy_base_schema_ = bundle.legacy_base_schema;
    upgrade_acknowledged_ = false;
  }

private:
  layerc::core::GameProfile profile_;
  layerc::platform::ModuleSidecar<layerc::core::VisualProfile> visual_;
  layerc::platform::ModuleSidecar<layerc::core::AutomationProfile> automation_;
  std::filesystem::path path_;
  bool legacy_base_schema_{};
  bool legacy_visual_schema_{};
  bool upgrade_acknowledged_{};
  bool dirty_{};
  EditorProfileHistory history_;
};

} // namespace layerc::editor
