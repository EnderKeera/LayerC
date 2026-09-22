#pragma once

#include "editor_history.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/profile_store.hpp"

#include <filesystem>
#include <optional>
#include <string>

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
    return visual_;
  }
  [[nodiscard]] const std::optional<layerc::core::VisualProfile> &
  visual() const noexcept {
    return visual_;
  }

  [[nodiscard]] std::optional<layerc::core::AutomationProfile> &
  automation() noexcept {
    return automation_;
  }
  [[nodiscard]] const std::optional<layerc::core::AutomationProfile> &
  automation() const noexcept {
    return automation_;
  }

  [[nodiscard]] layerc::platform::VisualSidecarState &visual_state() noexcept {
    return visual_state_;
  }
  [[nodiscard]] layerc::platform::VisualSidecarState
  visual_state() const noexcept {
    return visual_state_;
  }
  [[nodiscard]] std::string &visual_error() noexcept { return visual_error_; }
  [[nodiscard]] const std::string &visual_error() const noexcept {
    return visual_error_;
  }

  [[nodiscard]] layerc::platform::AutomationSidecarState &
  automation_state() noexcept {
    return automation_state_;
  }
  [[nodiscard]] layerc::platform::AutomationSidecarState
  automation_state() const noexcept {
    return automation_state_;
  }
  [[nodiscard]] std::string &automation_error() noexcept {
    return automation_error_;
  }
  [[nodiscard]] const std::string &automation_error() const noexcept {
    return automation_error_;
  }

  [[nodiscard]] std::filesystem::path &path() noexcept { return path_; }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] bool &legacy_base_schema() noexcept {
    return legacy_base_schema_;
  }
  [[nodiscard]] bool legacy_base_schema() const noexcept {
    return legacy_base_schema_;
  }
  [[nodiscard]] bool &legacy_visual_schema() noexcept {
    return legacy_visual_schema_;
  }
  [[nodiscard]] bool legacy_visual_schema() const noexcept {
    return legacy_visual_schema_;
  }
  [[nodiscard]] bool &upgrade_acknowledged() noexcept {
    return upgrade_acknowledged_;
  }
  [[nodiscard]] bool upgrade_acknowledged() const noexcept {
    return upgrade_acknowledged_;
  }
  [[nodiscard]] bool &dirty() noexcept { return dirty_; }
  [[nodiscard]] bool dirty() const noexcept { return dirty_; }
  [[nodiscard]] EditorProfileHistory &history() noexcept { return history_; }
  [[nodiscard]] const EditorProfileHistory &history() const noexcept {
    return history_;
  }

private:
  layerc::core::GameProfile profile_;
  std::optional<layerc::core::VisualProfile> visual_;
  std::optional<layerc::core::AutomationProfile> automation_;
  layerc::platform::AutomationSidecarState automation_state_{
      layerc::platform::AutomationSidecarState::absent};
  std::string automation_error_;
  layerc::platform::VisualSidecarState visual_state_{
      layerc::platform::VisualSidecarState::absent};
  std::string visual_error_;
  std::filesystem::path path_;
  bool legacy_base_schema_{};
  bool legacy_visual_schema_{};
  bool upgrade_acknowledged_{};
  bool dirty_{};
  EditorProfileHistory history_;
};

} // namespace layerc::editor
