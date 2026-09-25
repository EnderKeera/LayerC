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
#include <utility>

namespace layerc::editor {

enum class DocumentChangeKind { text, immediate, drag };

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
    return visual_.error.message;
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
  [[nodiscard]] const EditorProfileHistory &history() const noexcept {
    return history_;
  }

  // Sidecar value, state, and error move together so no caller can leave a
  // valid state beside a stale error or an invalid state beside a value.
  void set_visual(layerc::platform::VisualSidecarLoad loaded, const bool legacy) {
    visual_ = std::move(loaded);
    legacy_visual_schema_ = legacy;
  }
  void set_automation(layerc::platform::AutomationSidecarLoad loaded) {
    automation_ = std::move(loaded);
  }
  void mark_visual_edited() { mark_edited(visual_); }
  void mark_automation_edited() { mark_edited(automation_); }
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
    set_visual(std::move(bundle.visual), bundle.legacy_visual_schema);
    set_automation(std::move(bundle.automation));
    legacy_base_schema_ = bundle.legacy_base_schema;
    upgrade_acknowledged_ = false;
  }

private:
  // After an in-memory edit the sidecar is valid when present, else absent.
  template <typename Load> static void mark_edited(Load &load) {
    load.state = load.profile ? layerc::platform::ModuleSidecarState::valid
                              : layerc::platform::ModuleSidecarState::absent;
    load.error = {};
  }

  layerc::core::GameProfile profile_;
  layerc::platform::VisualSidecarLoad visual_;
  layerc::platform::AutomationSidecarLoad automation_;
  std::filesystem::path path_;
  bool legacy_base_schema_{};
  bool legacy_visual_schema_{};
  bool upgrade_acknowledged_{};
  bool dirty_{};
  EditorProfileHistory history_;
};

} // namespace layerc::editor
