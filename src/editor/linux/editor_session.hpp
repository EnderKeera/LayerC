#pragma once

#include "daemon_link.hpp"
#include "document_commit.hpp"
#include "profile_document.hpp"
#include "visual_assignment.hpp"
#include "layerc/bundle/profile_pack.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/visual_assets_linux.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::editor {

// Result of a store operation, with the text every frontend shows.
struct OperationOutcome {
  bool ok{};
  std::string text;
};

struct SessionSaveOutcome {
  CommitOutcome commit;
  std::optional<ReloadResult> reload;
  std::string text;
  [[nodiscard]] bool saved() const noexcept { return commit.ok(); }
};

// One reviewed pack import: rows the user can switch between copy, replace
// (only for a collision) and skip before committing.
struct ImportRow {
  std::string name;
  std::string filename;
  bool collides{};
  layerc::bundle::ImportAction action{layerc::bundle::ImportAction::copy};
};
struct ImportPlan {
  std::filesystem::path source;
  layerc::bundle::ValidatedPack pack;
  std::vector<ImportRow> rows;
  std::vector<layerc::bundle::ImportChoice> reviewed;
};

// Toolkit-free editor state for the Linux frontend: the profile list, the one
// open document (a saved profile or an unsaved draft), validated edits with
// undo/redo, durable saves, enablement, deletion and profile packs. Every
// write goes through the shared durable store transactions.
class EditorSession final {
public:
  explicit EditorSession(
      std::filesystem::path directory = layerc::platform::profile_directory());

  void rescan();
  [[nodiscard]] const layerc::platform::ProfileDirectoryScan &profiles() const {
    return scan_;
  }
  [[nodiscard]] const std::filesystem::path &directory() const { return directory_; }
  // The list row of the open document, when it is saved.
  [[nodiscard]] std::optional<std::size_t> current_row() const;

  [[nodiscard]] bool has_document() const noexcept { return opened_; }
  [[nodiscard]] bool is_draft() const noexcept { return opened_ && document_.path().empty(); }
  [[nodiscard]] ProfileDocument &document() noexcept { return document_; }
  [[nodiscard]] const ProfileDocument &document() const noexcept { return document_; }
  // Unsaved edits that would be lost (any draft counts).
  [[nodiscard]] bool has_unsaved_changes() const;
  // Changes only on accepted mutations or document replacement, not reports.
  [[nodiscard]] std::uint64_t edit_generation() const noexcept { return edit_generation_; }
  [[nodiscard]] bool last_edit_was_text() const noexcept { return last_edit_was_text_; }

  // Opening, creating and duplicating replace the document: callers ask
  // about unsaved changes first.
  [[nodiscard]] std::optional<std::string> open(std::size_t row);
  [[nodiscard]] std::optional<std::string> open_path(const std::filesystem::path &path);
  void new_profile();
  [[nodiscard]] std::optional<std::string> duplicate_current();
  void close_document();

  // Applies one edit to a copy of the profile; the complete candidate must
  // validate before it replaces the document. Text edits coalesce in history.
  [[nodiscard]] std::optional<std::string>
  edit(const std::function<void(layerc::core::GameProfile &)> &change,
       bool text_change = false);
  // Edits a module copy, validates the complete result against the base, and
  // records one normal history transaction. Unreadable sidecars stay blocked.
  [[nodiscard]] std::optional<std::string> edit_automation(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::AutomationProfile> &)> &change,
      bool text_change = false, bool repair = false);
  // Complete validated visual edits with shared undo/redo. Ordinary edits
  // cannot replace an unreadable sidecar; only explicit calibration can repair it.
  [[nodiscard]] std::optional<std::string> edit_visual(
      const std::function<std::optional<std::string>(
          std::optional<layerc::core::VisualProfile> &)> &change,
      bool text_change = false);
  // Takes ownership of the calibration's new template bytes; the next Save
  // publishes them with the profile and sidecars in one durable transaction.
  [[nodiscard]] std::optional<std::string> apply_calibration(
      std::string_view cooldown_id, std::string_view visual_json,
      const std::function<SharedObservationChoice()> &choose_shared,
      std::vector<layerc::platform::EncodedTemplateAsset> assets = {});
  [[nodiscard]] const std::vector<layerc::platform::EncodedTemplateAsset> &
  owned_template_assets() const noexcept {
    return owned_assets_;
  }
  [[nodiscard]] std::optional<std::string>
  set_visual_runtime(layerc::core::VisualRuntime runtime);
  // Removes a cooldown with its bindings, panel memberships and module
  // references, refusing when a group or rule still depends on it.
  [[nodiscard]] std::optional<std::string> remove_cooldown(std::string_view id);
  void end_text_edit();
  [[nodiscard]] bool can_undo() const { return document_.history().can_undo(); }
  [[nodiscard]] bool can_redo() const { return document_.history().can_redo(); }
  bool undo();
  bool redo();

  [[nodiscard]] SessionSaveOutcome save(const ConfirmUpgrade &confirm_upgrade,
                                        bool prompt_upgrade = true);
  // Enables or disables a saved profile on disk. The open document must
  // have no unsaved edits, so enablement never publishes them implicitly.
  [[nodiscard]] OperationOutcome set_enabled(std::size_t row, bool enabled,
                                             const ConfirmUpgrade &confirm_upgrade);
  // Deletes the open saved profile with every module file in one durable
  // transaction; a draft is simply discarded.
  [[nodiscard]] OperationOutcome delete_current();

  // JSON exports contain only the saved base profile; packs include modules.
  [[nodiscard]] OperationOutcome export_json(const std::filesystem::path &destination,
                                             std::size_t row);
  [[nodiscard]] OperationOutcome export_pack(const std::filesystem::path &destination,
                                             const std::vector<std::size_t> &rows);
  [[nodiscard]] std::variant<ImportPlan, std::string>
  plan_import(const std::filesystem::path &source);
  // Rebind the entire winning effect set after any action changes. The UI
  // displays the returned destinations before accepting this plan.
  [[nodiscard]] std::variant<ImportPlan, std::string> review_import(ImportPlan plan);
  [[nodiscard]] OperationOutcome import_pack(const ImportPlan &plan);

private:
  void mark_changed(bool text_change = false);
  void restore_visual_history(std::optional<layerc::core::VisualProfile> visual);
  void accept_visual(std::optional<layerc::core::VisualProfile> visual,
                     bool text_change = false);
  // Drops owned template bytes that neither the document nor any snapshot
  // history can restore still references.
  void release_unreachable_assets();
  [[nodiscard]] OperationOutcome reload_report(const std::string &done);

  std::filesystem::path directory_;
  layerc::platform::ProfileDirectoryScan scan_;
  ProfileDocument document_;
  // Read-only companion data is kept for a duplicate, never edited by this UI.
  layerc::platform::CompanionSidecarLoad companion_;
  // An unsaved repair can be undone back to the original unreadable state,
  // not to a deletion intent. Cleared only after a successful commit or reopen.
  layerc::platform::VisualSidecarLoad unreadable_visual_;
  // Template bytes this document owns that its store may not hold yet: a
  // duplicate's original encoded identities and applied calibration images.
  // Save publishes the referenced ones inside its bundle transaction; the rest
  // stay owned until the document closes, so Undo can restore references that
  // a later Save still publishes, even after the source namespace is deleted.
  std::vector<layerc::platform::EncodedTemplateAsset> owned_assets_;
  std::uint64_t edit_generation_{};
  bool last_edit_was_text_{};
  bool opened_{};
};

} // namespace layerc::editor
