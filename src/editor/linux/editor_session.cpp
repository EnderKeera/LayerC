#include "editor_session.hpp"

#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>

namespace layerc::editor {
namespace {

namespace core = layerc::core;
namespace platform = layerc::platform;
namespace bundle = layerc::bundle;

bool references_template(const core::VisualProfile &visual, const std::string_view sha256) {
  return std::ranges::any_of(visual.observations, [&](const auto &observation) {
    const auto *state = std::get_if<core::TemplateState>(&observation);
    return state && std::ranges::any_of(state->references, [&](const auto &reference) {
             return reference.asset_sha256 == sha256;
           });
  });
}

// Every file that belongs to one stored profile: the base file, its recovery
// copy and each module sidecar with its backup and marker.
std::vector<std::filesystem::path> bundle_files(const std::filesystem::path &path) {
  std::vector<std::filesystem::path> files{path, platform::recovery_backup_path(path)};
  for (const auto &module : core::ProfileModuleTable::descriptors) {
    const auto sidecar = platform::sidecar_paths(path, module.kind);
    files.insert(files.end(), {sidecar.primary, sidecar.backup, sidecar.marker});
  }
  const auto state = platform::automation_state_path(path);
  files.insert(files.end(), {state, platform::recovery_backup_path(state)});
  return files;
}

bool export_destination_allowed(const std::filesystem::path &destination,
                                const std::filesystem::path &directory) {
  const auto roots = platform::profile_transaction_scope(directory);
  if (!roots || destination.empty()) return false;
  const std::array protected_roots{roots->config_profiles, roots->data_profiles,
                                   roots->store};
  std::error_code error;
  const auto absolute = std::filesystem::absolute(destination, error);
  if (error) return false;
  for (auto parent = absolute; !parent.empty();) {
    for (const auto &root : protected_roots)
      if (platform::canonical_profile_key(parent) == platform::canonical_profile_key(root))
        return false;
    const auto next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
  return true;
}

std::string pack_error_text(const char *what, const bundle::PackError &error) {
  return std::string(what) + ": " + error.message;
}

} // namespace

EditorSession::EditorSession(std::filesystem::path directory)
    : directory_(std::move(directory)) {
  rescan();
}

void EditorSession::rescan() {
  scan_ = platform::scan_profile_summaries(directory_);
}

std::optional<std::size_t> EditorSession::current_row() const {
  if (!opened_ || document_.path().empty()) return std::nullopt;
  for (std::size_t row = 0; row < scan_.profiles.size(); ++row)
    if (is_document_path(document_, scan_.profiles[row].path)) return row;
  return std::nullopt;
}

bool EditorSession::has_unsaved_changes() const {
  if (!opened_) return false;
  return is_draft() || document_.dirty();
}

std::optional<std::string> EditorSession::open(const std::size_t row) {
  if (!scan_.error.empty()) return scan_.error;
  if (row >= scan_.profiles.size()) return "No profile selected.";
  auto loaded = platform::load_profile_bundle(scan_.profiles[row].path);
  auto *loaded_bundle = std::get_if<platform::ProfileBundleLoad>(&loaded);
  if (!loaded_bundle)
    return "Could not read the selected profile; its files were not changed.";
  companion_ = loaded_bundle->modules.get<core::CompanionModule>();
  // The document keeps an unreadable visual sidecar itself, so undo can restore
  // exactly its state and diagnostic after an explicit calibration repair.
  document_.open(scan_.profiles[row].path, std::move(*loaded_bundle));
  owned_assets_.clear();
  opened_ = true;
  mark_changed(false);
  return std::nullopt;
}

std::optional<std::string> EditorSession::open_path(const std::filesystem::path &path) {
  for (std::size_t row = 0; row < scan_.profiles.size(); ++row)
    if (scan_.profiles[row].path == path) return open(row);
  return "The selected profile is no longer in the profile list.";
}

void EditorSession::new_profile() {
  owned_assets_.clear();
  companion_ = {};
  rescan();
  auto profile = make_new_profile();
  profile.id = next_unique_id(profile.id, [&](const std::string &id) {
    return scan_.find(id) != nullptr;
  });
  document_.start_draft(std::move(profile), DraftOrigin::fresh);
  opened_ = true;
  mark_changed(false);
}

std::optional<std::string> EditorSession::duplicate_current() {
  if (!opened_) return "No profile is open.";
  if (document_.has_unreadable_module() ||
      companion_.state() == platform::ModuleSidecarState::invalid)
    return "Not duplicated: an unreadable module cannot be preserved in a copy.";
  const auto taken = [&](const std::string_view id) {
    return std::ranges::any_of(scan_.profiles, [&](const auto &stored) {
             return stored.profile.id == id;
           }) ||
           document_.profile().id == id;
  };
  auto copy = duplicate_profile(document_.profile(), taken);
  if (const auto error = core::validate_profile(copy))
    return "Not duplicated: " + profile_error_line(*error);
  std::vector<platform::EncodedTemplateAsset> assets;
  if (document_.visual()) {
    auto snapshot = platform::snapshot_profile_template_assets(
        document_.path(), *document_.visual(), owned_assets_);
    if (const auto *error = std::get_if<platform::VisualAssetError>(&snapshot))
      return "Not duplicated: " + error->message;
    assets = std::get<std::vector<platform::EncodedTemplateAsset>>(std::move(snapshot));
  }
  if (auto companion = companion_.optional_profile()) {
    companion->profile_id = copy.id;
    companion_ = platform::CompanionSidecarLoad::valid(std::move(*companion), {});
  }
  auto visual = document_.visual();
  auto automation = document_.automation();
  if (visual) visual->profile_id = copy.id;
  if (automation) automation->profile_id = copy.id;
  owned_assets_ = std::move(assets);
  // The copy keeps editable modules; their profile ids realign at commit, and a
  // copy is unsaved by construction.
  document_.start_draft(std::move(copy), DraftOrigin::copy, std::move(visual),
                        std::move(automation));
  mark_changed(false);
  return std::nullopt;
}

void EditorSession::close_document() {
  owned_assets_.clear();
  document_.close();
  companion_ = {};
  opened_ = false;
  mark_changed(false);
}

void EditorSession::mark_changed(const bool text_change) {
  ++edit_generation_;
  last_edit_was_text_ = text_change;
  if (change_listener_) change_listener_();
}

void EditorSession::set_change_listener(std::function<void()> listener) {
  change_listener_ = std::move(listener);
}

std::optional<std::string>
EditorSession::edit(const std::function<void(core::GameProfile &)> &change,
                    const bool text_change) {
  if (!opened_) return "No profile is open.";
  const auto outcome = document_.edit_profile(
      change, text_change ? DocumentChangeKind::text : DocumentChangeKind::immediate,
      companion_.profile());
  if (outcome.accepted()) mark_changed(text_change);
  if (outcome.accepted() || outcome.status == ModuleEditStatus::unchanged)
    return std::nullopt;
  return outcome.message;
}

std::optional<std::string> EditorSession::edit_automation(
    const std::function<std::optional<std::string>(
        std::optional<core::AutomationProfile> &)> &change,
    const bool text_change, const bool repair) {
  if (!opened_) return "No profile is open.";
  const auto outcome = document_.edit_automation(
      change, text_change ? DocumentChangeKind::text : DocumentChangeKind::immediate,
      repair);
  if (outcome.accepted()) mark_changed(text_change);
  if (outcome.accepted() || outcome.status == ModuleEditStatus::unchanged)
    return std::nullopt;
  return outcome.message;
}

void EditorSession::release_unreachable_assets() {
  std::erase_if(owned_assets_, [&](const platform::EncodedTemplateAsset &asset) {
    bool reachable{};
    document_.for_each_retained_visual(
        [&](const std::optional<core::VisualProfile> &visual) {
          reachable = reachable ||
                      (visual && references_template(*visual, asset.asset_sha256));
        });
    return !reachable;
  });
}

std::optional<std::string> EditorSession::edit_visual(
    const std::function<std::optional<std::string>(
        std::optional<core::VisualProfile> &)> &change,
    const bool text_change) {
  if (!opened_) return "No profile is open.";
  const auto outcome = document_.edit_visual(
      change, text_change ? DocumentChangeKind::text : DocumentChangeKind::immediate);
  if (outcome.accepted()) {
    release_unreachable_assets();
    mark_changed(text_change);
  }
  if (outcome.accepted() || outcome.status == ModuleEditStatus::unchanged)
    return std::nullopt;
  return outcome.message;
}

CalibrationAcceptance EditorSession::prepare_calibration(
    const std::string_view cooldown_id,
    const core::VisualProfile &calibration,
    const std::span<const core::TemplateImage> images,
    const std::function<SharedObservationChoice()> &choose_shared) const {
  if (!opened_) return std::string("No profile is open.");
  const auto generation = edit_generation_;
  const auto path = document_.path();
  auto merged = merge_calibration_result(
      document_.visual(),
      document_.visual_state() == platform::ModuleSidecarState::invalid,
      document_.profile(), cooldown_id, calibration, choose_shared);
  // A nested shared-choice dialog can dispatch another editor command.
  if (!opened_ || edit_generation_ != generation || document_.path() != path)
    return std::string("The active document changed during calibration.");
  if (const auto *refusal = std::get_if<VisualMergeRefusal>(&merged))
    return refusal->message;
  auto prepared = platform::prepare_profile_template_assets(path, images, owned_assets_);
  if (const auto *error = std::get_if<platform::VisualAssetError>(&prepared))
    return error->message;
  return AcceptedCalibration{
      generation, path,
      std::get<core::VisualProfile>(std::move(merged)),
      std::get<std::vector<platform::EncodedTemplateAsset>>(std::move(prepared))};
}

std::optional<std::string>
EditorSession::apply_calibration(AcceptedCalibration accepted) {
  if (!opened_ || accepted.generation != edit_generation_ ||
      accepted.path != document_.path())
    return "The active document changed during calibration.";
  // The document validates against the current profile and records one step;
  // an invalid result returns its message before any byte is adopted.
  const auto outcome =
      document_.replace_visual_from_calibration(std::move(accepted.visual));
  // An identical recalibration is a success: nothing changed, no new step and
  // no new bytes are adopted for a module whose references are unchanged.
  if (outcome.status == ModuleEditStatus::unchanged) return std::nullopt;
  if (!outcome.accepted()) return outcome.message;
  // All refusals above are inert. Bytes remain owned until transactional Save.
  for (auto &asset : accepted.assets)
    if (std::ranges::find(owned_assets_, asset.asset_sha256,
          &platform::EncodedTemplateAsset::asset_sha256) == owned_assets_.end())
      owned_assets_.push_back(std::move(asset));
  release_unreachable_assets();
  mark_changed();
  return std::nullopt;
}

std::optional<std::string>
EditorSession::set_visual_runtime(const core::VisualRuntime runtime) {
  if (runtime != core::VisualRuntime::inline_process &&
      runtime != core::VisualRuntime::isolated)
    return "Unknown visual runtime.";
  return edit([&](core::GameProfile &profile) { profile.visual_runtime = runtime; });
}

std::optional<std::string> EditorSession::remove_cooldown(const std::string_view id) {
  if (!opened_) return "No profile is open.";
  if (const auto error = document_.remove_cooldown(id)) return error->message;
  mark_changed();
  return std::nullopt;
}

void EditorSession::end_text_edit() { document_.end_text_coalescing(); }

bool EditorSession::undo() {
  if (!document_.undo()) return false;
  mark_changed();
  return true;
}

bool EditorSession::redo() {
  if (!document_.redo()) return false;
  mark_changed();
  return true;
}

SessionSaveOutcome EditorSession::save(const ConfirmUpgrade &confirm_upgrade,
                                        const SaveMode mode) {
  const bool autosave = mode == SaveMode::autosave;
  if (!opened_) return {{CommitResult::invalid_fields, std::nullopt}, std::nullopt,
                        autosave ? "Not autosaved: no profile is open."
                                 : "Not saved: no profile is open."};
  const bool draft = is_draft();
  const auto path = draft ? default_profile_path(directory_, document_.profile())
                          : document_.path();
  platform::ModuleIntent<core::CompanionProfile> companion = platform::KeepModule{};
  if (draft && companion_.profile()) companion = *companion_.profile();
  // Owned template bytes publish inside this bundle's transaction. A draft
  // must own every reference; a stored profile already holds the others.
  std::vector<platform::EncodedTemplateAsset> assets;
  if (document_.visual() && draft) {
    auto snapshot = platform::snapshot_profile_template_assets(
        {}, *document_.visual(), owned_assets_);
    if (const auto *error = std::get_if<platform::VisualAssetError>(&snapshot))
      return {{CommitResult::invalid_fields, std::nullopt}, std::nullopt,
              (autosave ? "Not autosaved: " : "Not saved: ") + error->message};
    assets = std::get<std::vector<platform::EncodedTemplateAsset>>(std::move(snapshot));
  } else if (document_.visual()) {
    for (const auto &asset : owned_assets_)
      if (references_template(*document_.visual(), asset.asset_sha256)) assets.push_back(asset);
  }
  const auto commit = commit_document(document_, path,
      {.prompt_upgrade = !autosave, .create_only = draft},
      confirm_upgrade, std::move(companion), [&](auto &mutations) {
        return platform::plan_profile_template_copies(path, assets, mutations);
      });
  std::optional<ReloadResult> reload;
  if (commit.ok()) {
    // Published bytes now live in the store; unpublished ones stay owned
    // while history can still restore a reference to them.
    std::erase_if(owned_assets_, [&](const platform::EncodedTemplateAsset &owned) {
      return std::ranges::find(assets, owned.asset_sha256,
                               &platform::EncodedTemplateAsset::asset_sha256) != assets.end();
    });
    release_unreachable_assets();
    reload = request_daemon_reload();
    rescan();
    mark_changed(false);
  }
  return {commit, reload, save_report_text(commit, reload, mode)};
}

OperationOutcome EditorSession::reload_report(const std::string &done) {
  rescan();
  const auto reload = request_daemon_reload();
  if (reload.state == ReloadState::acknowledged)
    return {true, done + " LayerD acknowledged the reload request."};
  if (reload.state == ReloadState::daemon_unavailable)
    return {true, done + " LayerD is not running; it will load the change when it starts."};
  return {true, done + " LayerD's reload could not be confirmed: " + reload.error};
}

OperationOutcome EditorSession::set_enabled(const std::size_t row, const bool enabled,
                                            const ConfirmUpgrade &confirm_upgrade) {
  if (row >= scan_.profiles.size()) return {false, "No profile selected."};
  const auto path = scan_.profiles[row].path;
  const bool is_current = opened_ && is_document_path(document_, path);
  auto selected = profile_enablement_bundle(
      document_, is_current, path, OpenDocumentEnablement::require_clean);
  if (const auto *refusal = std::get_if<EnablementRefusal>(&selected)) {
    if (*refusal == EnablementRefusal::dirty_open_document)
      return {false, "Save or undo the open profile's changes before turning it on or off."};
    return {false, "Could not read that profile; it was not changed."};
  }
  auto candidate = std::get<ProfileBundle>(std::move(selected));
  if (candidate.profile.enabled == enabled) return {true, "No change."};
  candidate.profile.enabled = enabled;
  const auto commit = commit_profile_bundle(path, candidate, {}, confirm_upgrade);
  if (!commit.ok())
    return {false, commit.result == CommitResult::write_failed
                       ? "Not changed. " + write_failure_text(commit.write_error) + "."
                       : std::string("Not changed: the profile or its modules need repair.")};
  auto outcome = reload_report(enabled ? "Profile enabled." : "Profile disabled.");
  if (is_current) {
    if (const auto error = open_path(path)) {
      close_document();
      outcome.text += " Could not reopen the saved profile: " + *error;
    }
  }
  return outcome;
}

OperationOutcome EditorSession::delete_current() {
  if (!opened_) return {false, "No profile is open."};
  if (is_draft()) {
    close_document();
    return {true, "Draft discarded."};
  }
  const auto path = document_.path();
  const auto planner = [&](std::vector<platform::ProfileFileMutation> &mutations)
      -> platform::ProfileWriteResult {
    std::error_code error;
    for (const auto &file : bundle_files(path)) {
      if (file.empty()) continue;
      const auto status = std::filesystem::symlink_status(file, error);
      if (error && status.type() != std::filesystem::file_type::not_found)
        return {platform::ProfileWriteError::io_failed};
      if (status.type() != std::filesystem::file_type::not_found)
        mutations.push_back({file, std::nullopt, false});
    }
    if (mutations.empty()) return {platform::ProfileWriteError::invalid_input};
    return {};
  };
  const auto result = platform::commit_profile_transaction(
      platform::ProfileTransactionScope(directory_), platform::ProfileMutationPlan(planner));
  if (!result.ok())
    return {false, std::string("Not deleted: ") +
                       platform::profile_write_error_text(*result.failure) + "."};
  close_document();
  return reload_report("Profile deleted.");
}

OperationOutcome EditorSession::export_json(const std::filesystem::path &destination,
                                            const std::size_t row) {
  if (!export_destination_allowed(destination, directory_))
    return {false, "Export failed: choose a destination outside the live profile store."};
  if (row >= scan_.profiles.size()) return {false, "No saved profile selected."};
  const auto bytes = platform::read_profile_file(scan_.profiles[row].path);
  if (!bytes || !std::holds_alternative<core::GameProfile>(core::parse_profile_json(*bytes)))
    return {false, "Export failed: the saved base profile is unreadable or invalid."};
  const auto published = platform::write_durable_file(destination, core::owned_bytes(*bytes));
  if (published == platform::DurablePublishOutcome::failed)
    return {false, "JSON export failed; the destination was not changed."};
  if (published == platform::DurablePublishOutcome::published_unconfirmed)
    return {false, "JSON is exported, but its crash durability could not be confirmed."};
  return {true, "Exported the saved base JSON (without module sidecars). Use a profile "
                "pack to export the complete bundle."};
}

OperationOutcome EditorSession::export_pack(const std::filesystem::path &destination,
                                            const std::vector<std::size_t> &rows) {
  if (!export_destination_allowed(destination, directory_))
    return {false, "Export failed: choose a destination outside the live profile store."};
  std::vector<std::filesystem::path> paths;
  for (const auto row : rows) {
    if (row >= scan_.profiles.size())
      return {false, "Export failed: the profile selection is no longer valid."};
    paths.push_back(scan_.profiles[row].path);
  }
  if (paths.empty()) return {false, "Select at least one saved profile to export."};
  const auto result = bundle::export_profile_pack(destination, paths);
  if (const auto *error = std::get_if<bundle::PackError>(&result))
    return {false, pack_error_text("Export failed", *error)};
  return {true, "Exported " + std::to_string(paths.size()) + " profile(s) to " +
                    destination.filename().string() + "."};
}

std::variant<ImportPlan, std::string>
EditorSession::plan_import(const std::filesystem::path &source) {
  bundle::PackValidationResult validation;
  const bool base_only = platform::has_json_extension(source);
  if (base_only) {
    const auto bytes = platform::read_profile_file(source);
    if (!bytes) return std::string("Import failed: the JSON file could not be read.");
    auto parsed = core::parse_profile_json(*bytes);
    auto *profile = std::get_if<core::GameProfile>(&parsed);
    if (!profile) return "Import failed: " + profile_error_line(std::get<core::ProfileError>(parsed));
    bundle::PackProfile item{
        .suggested_filename = platform::path_to_utf8(source.filename()).value_or("profile.json"),
        .base_json = *bytes, .profile = std::move(*profile), .modules = {},
        .asset_sha256 = {}};
    validation = bundle::ValidatedPack{.producer_version = {},
                                      .profiles = {std::move(item)}, .assets = {}};
  } else {
    validation = bundle::validate_profile_pack(source);
  }
  auto *pack = std::get_if<bundle::ValidatedPack>(&validation);
  if (!pack)
    return pack_error_text("Not a valid profile pack",
                           std::get<bundle::PackError>(validation));
  std::vector<bundle::ImportChoice> copies;
  for (std::size_t index = 0; index < pack->profiles.size(); ++index)
    copies.push_back({.profile_index = index, .action = bundle::ImportAction::copy,
                      .keep_missing_modules = base_only});
  const auto preview = bundle::preview_profile_import(*pack, copies, directory_);
  if (preview.error) return pack_error_text("Import preview failed", *preview.error);
  ImportPlan plan{source, std::move(*pack), {}, preview.choices};
  for (const auto &entry : preview.entries)
    plan.rows.push_back({plan.pack.profiles[entry.profile_index].profile.name,
                         entry.target.filename().string(), entry.collides,
                         bundle::ImportAction::copy});
  return plan;
}

std::variant<ImportPlan, std::string> EditorSession::review_import(ImportPlan plan) {
  if (plan.rows.size() != plan.reviewed.size())
    return std::string("Import failed: the reviewed plan is incomplete.");
  auto choices = plan.reviewed;
  for (std::size_t index = 0; index < plan.rows.size(); ++index) {
    choices[index].action = plan.rows[index].action;
    choices[index].expected_target.reset();
    choices[index].require_absent = false;
  }
  const auto preview = bundle::preview_profile_import(plan.pack, choices, directory_);
  if (preview.error) return pack_error_text("Import preview failed", *preview.error);
  plan.reviewed = preview.choices;
  for (std::size_t index = 0; index < preview.entries.size(); ++index) {
    plan.rows[index].filename = preview.entries[index].target.filename().string();
    plan.rows[index].collides = preview.entries[index].collides;
  }
  return plan;
}

OperationOutcome EditorSession::import_pack(const ImportPlan &plan) {
  if (plan.rows.size() != plan.reviewed.size())
    return {false, "Import failed: the reviewed plan is incomplete."};
  std::vector<bundle::ImportChoice> choices;
  std::size_t imported = 0;
  for (std::size_t index = 0; index < plan.rows.size(); ++index) {
    auto choice = plan.reviewed[index];
    if (choice.action != plan.rows[index].action || !choice.expected_target)
      return {false, "Import actions changed; review their destinations again."};
    if (choice.action != bundle::ImportAction::skip) ++imported;
    choices.push_back(std::move(choice));
  }
  if (imported == 0) return {true, "Nothing imported."};
  const bool replaces_current = opened_ && !is_draft() &&
      std::ranges::any_of(choices, [&](const auto &choice) {
        return choice.action != bundle::ImportAction::skip &&
               is_document_path(document_, *choice.expected_target);
      });
  if (replaces_current && has_unsaved_changes())
    return {false, "Save or discard the open profile's edits before replacing it."};
  const auto path = document_.path();
  const auto result = bundle::import_profile_pack(plan.pack, choices, directory_);
  if (const auto *error = std::get_if<bundle::PackError>(&result))
    return {false, pack_error_text("Import failed", *error)};
  auto outcome = reload_report("Imported " + std::to_string(imported) + " profile(s).");
  if (replaces_current) {
    if (const auto error = open_path(path)) {
      close_document();
      outcome.text += " Could not reopen the replaced profile: " + *error;
    }
  }
  return outcome;
}

} // namespace layerc::editor
