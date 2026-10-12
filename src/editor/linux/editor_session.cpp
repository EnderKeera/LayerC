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

void EditorSession::rescan() { scan_ = platform::scan_profile_directory(directory_); }

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
  const auto &visual = loaded_bundle->modules.get<core::VisualModule>();
  unreadable_visual_ = visual.state() == platform::ModuleSidecarState::invalid
                           ? visual : platform::VisualSidecarLoad{};
  document_.assign_bundle(scan_.profiles[row].path, std::move(*loaded_bundle));
  owned_assets_.clear();
  document_.set_dirty(false);
  document_.reset_history();
  opened_ = true;
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
  unreadable_visual_ = {};
  document_.path().clear();
  document_.clear_sidecars();
  document_.clear_legacy_schemas();
  rescan();
  document_.profile() = make_new_profile();
  document_.profile().id = next_unique_id(document_.profile().id, [&](const std::string &id) {
    return scan_.find(id) != nullptr;
  });
  document_.reset_history();
  document_.set_dirty(true);
  opened_ = true;
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
  document_.path().clear();
  document_.clear_sidecars();
  document_.clear_legacy_schemas();
  document_.profile() = std::move(copy);
  owned_assets_ = std::move(assets);
  // The copy keeps editable modules; their profile ids realign at commit.
  document_.visual() = std::move(visual);
  document_.automation() = std::move(automation);
  document_.mark_visual_edited();
  document_.mark_automation_edited();
  document_.reset_history();
  document_.set_dirty(true);
  return std::nullopt;
}

void EditorSession::close_document() {
  owned_assets_.clear();
  document_ = ProfileDocument{};
  companion_ = {};
  unreadable_visual_ = {};
  opened_ = false;
}

void EditorSession::mark_changed() {
  document_.set_dirty(is_draft() ||
                      document_.history().is_dirty(document_.profile(), document_.visual(),
                                                   document_.automation()));
}

std::optional<std::string>
EditorSession::edit(const std::function<void(core::GameProfile &)> &change,
                    const bool text_change) {
  if (!opened_) return "No profile is open.";
  auto candidate = document_.profile();
  change(candidate);
  if (const auto error = core::validate_profile(candidate))
    return profile_error_line(*error);
  if (candidate == document_.profile()) return std::nullopt;
  if (document_.visual() && !core::VisualModule::valid(*document_.visual(), candidate))
    return "The edit would invalidate the existing visual module.";
  if (document_.automation() && !core::AutomationModule::valid(*document_.automation(), candidate))
    return "The edit would invalidate the existing automation module.";
  if (companion_.profile() && !core::CompanionModule::valid(*companion_.profile(), candidate))
    return "The edit would invalidate the read-only Game Bar companion module.";
  document_.profile() = std::move(candidate);
  (void)document_.history().record(document_.profile(), document_.visual(),
                                    document_.automation(), text_change);
  mark_changed();
  return std::nullopt;
}

std::optional<std::string> EditorSession::edit_automation(
    const std::function<std::optional<std::string>(
        std::optional<core::AutomationProfile> &)> &change,
    const bool text_change, const bool repair) {
  if (!opened_) return "No profile is open.";
  if (document_.automation_state() == platform::ModuleSidecarState::invalid)
    return "The unreadable automation module is preserved; editing is blocked.";
  auto candidate = document_.automation();
  // Removals may repair an already-invalid graph one node at a time.
  const bool was_invalid = repair && candidate &&
      !core::validate_automation_profile(*candidate, document_.profile()).message.empty();
  if (const auto error = change(candidate)) return error;
  if (candidate && !was_invalid) {
    const auto error = core::validate_automation_profile(*candidate, document_.profile());
    if (!error.message.empty()) return error.describe();
  }
  if (candidate == document_.automation()) return std::nullopt;
  document_.automation() = std::move(candidate);
  document_.mark_automation_edited();
  (void)document_.history().record(document_.profile(), document_.visual(),
                                    document_.automation(), text_change);
  mark_changed();
  return std::nullopt;
}

void EditorSession::accept_visual(std::optional<core::VisualProfile> visual,
                                  const bool text_change) {
  document_.visual() = std::move(visual);
  document_.mark_visual_edited();
  (void)document_.history().record(document_.profile(), document_.visual(),
                                  document_.automation(), text_change);
  release_unreachable_assets();
  mark_changed();
}

void EditorSession::release_unreachable_assets() {
  std::erase_if(owned_assets_, [&](const platform::EncodedTemplateAsset &asset) {
    const auto uses = [&](const std::optional<core::VisualProfile> &visual) {
      return visual && references_template(*visual, asset.asset_sha256);
    };
    bool reachable = uses(document_.visual());
    document_.history().for_each_snapshot([&](const EditorProfileSnapshot &snapshot) {
      reachable = reachable || uses(snapshot.visual_profile);
    });
    return !reachable;
  });
}

std::optional<std::string> EditorSession::edit_visual(
    const std::function<std::optional<std::string>(
        std::optional<core::VisualProfile> &)> &change,
    const bool text_change) {
  if (!opened_) return "No profile is open.";
  if (document_.visual_state() == platform::ModuleSidecarState::invalid)
    return "The unreadable visual module is preserved; recalibrate to repair it.";
  auto candidate = document_.visual();
  if (const auto error = change(candidate)) return error;
  if (candidate) {
    const auto error = core::validate_visual_profile(*candidate, document_.profile());
    if (!error.message.empty()) return error.describe();
  }
  if (candidate != document_.visual()) accept_visual(std::move(candidate), text_change);
  return std::nullopt;
}

std::optional<std::string> EditorSession::apply_calibration(
    const std::string_view cooldown_id, const std::string_view visual_json,
    const std::function<SharedObservationChoice()> &choose_shared,
    std::vector<platform::EncodedTemplateAsset> assets) {
  if (!opened_) return "No profile is open.";
  auto merged = merge_calibration_result(document_.visual(),
      document_.visual_state() == platform::ModuleSidecarState::invalid,
      document_.profile(), cooldown_id, visual_json, choose_shared);
  if (const auto *refusal = std::get_if<VisualMergeRefusal>(&merged))
    return refusal->message;
  for (auto &asset : assets)
    if (std::ranges::find(owned_assets_, asset.asset_sha256,
                          &platform::EncodedTemplateAsset::asset_sha256) == owned_assets_.end())
      owned_assets_.push_back(std::move(asset));
  auto candidate = std::get<core::VisualProfile>(std::move(merged));
  if (candidate != document_.visual()) accept_visual(std::move(candidate));
  return std::nullopt;
}

std::optional<std::string>
EditorSession::set_visual_runtime(const core::VisualRuntime runtime) {
  if (runtime != core::VisualRuntime::inline_process &&
      runtime != core::VisualRuntime::isolated)
    return "Unknown visual runtime.";
  return edit([&](core::GameProfile &profile) { profile.visual_runtime = runtime; });
}

void EditorSession::restore_visual_history(std::optional<core::VisualProfile> visual) {
  if (document_.visual() == visual) return;
  document_.visual() = std::move(visual);
  if (!document_.visual() &&
      unreadable_visual_.state() == platform::ModuleSidecarState::invalid)
    document_.set_visual(unreadable_visual_, document_.legacy_visual_schema());
  else
    document_.mark_visual_edited();
}

std::optional<std::string> EditorSession::remove_cooldown(const std::string_view id) {
  if (!opened_) return "No profile is open.";
  if (const auto error = document_.remove_cooldown(id)) return error->message;
  (void)document_.history().record(document_.profile(), document_.visual(),
                                    document_.automation());
  mark_changed();
  return std::nullopt;
}

void EditorSession::end_text_edit() { document_.history().end_text_coalescing(); }

bool EditorSession::undo() {
  auto snapshot = document_.history().undo(document_.profile(), document_.visual(),
                                           document_.automation());
  if (!snapshot) return false;
  document_.profile() = std::move(snapshot->profile);
  restore_visual_history(std::move(snapshot->visual_profile));
  if (document_.automation() != snapshot->automation_profile) {
    document_.automation() = std::move(snapshot->automation_profile);
    document_.mark_automation_edited();
  }
  mark_changed();
  return true;
}

bool EditorSession::redo() {
  auto snapshot = document_.history().redo(document_.profile(), document_.visual(),
                                           document_.automation());
  if (!snapshot) return false;
  document_.profile() = std::move(snapshot->profile);
  restore_visual_history(std::move(snapshot->visual_profile));
  if (document_.automation() != snapshot->automation_profile) {
    document_.automation() = std::move(snapshot->automation_profile);
    document_.mark_automation_edited();
  }
  mark_changed();
  return true;
}

SessionSaveOutcome EditorSession::save(const ConfirmUpgrade &confirm_upgrade) {
  if (!opened_) return {{CommitResult::invalid_fields, std::nullopt}, std::nullopt,
                        "Not saved: no profile is open."};
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
              "Not saved: " + error->message};
    assets = std::get<std::vector<platform::EncodedTemplateAsset>>(std::move(snapshot));
  } else if (document_.visual()) {
    for (const auto &asset : owned_assets_)
      if (references_template(*document_.visual(), asset.asset_sha256)) assets.push_back(asset);
  }
  const auto commit = commit_document(document_, path, {.create_only = draft},
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
    unreadable_visual_ = {};
    document_.path() = path;
    document_.mark_saved();
    release_unreachable_assets();
    reload = request_daemon_reload();
    rescan();
  }
  return {commit, reload, save_report_text(commit, reload)};
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
  if (is_current && document_.dirty())
    return {false, "Save or undo the open profile's changes before turning it on or off."};
  auto loaded = platform::load_profile_bundle(path);
  auto *stored = std::get_if<platform::ProfileBundleLoad>(&loaded);
  if (!stored) return {false, "Could not read that profile; it was not changed."};
  auto candidate = loaded_bundle(std::move(*stored));
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
