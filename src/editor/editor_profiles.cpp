#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>

namespace layerc::editor {
namespace {

bool document_needs_schema_upgrade(const ProfileDocument &document) {
  return (document.legacy_base_schema() || document.legacy_visual_schema()) &&
         !document.upgrade_acknowledged();
}

bool is_document_path(const ProfileDocument &document,
                      const std::filesystem::path &path) {
  return !document.path().empty() &&
         layerc::platform::canonical_profile_key(path) ==
             layerc::platform::canonical_profile_key(document.path());
}

std::filesystem::path default_profile_filename(
    const layerc::core::GameProfile &profile) {
  return layerc::platform::profile_store_filename(profile.id);
}

void show_invalid_field_message(const HWND owner) {
  const auto limit = std::chrono::duration_cast<std::chrono::seconds>(
      layerc::core::max_cooldown_duration);
  const std::wstring message =
      L"Finish the invalid field. Cooldown duration must be greater than 0 "
      L"and no more than " +
      std::to_wstring(limit.count()) + L" seconds.";
  MessageBoxW(owner, message.c_str(), L"Profile is incomplete",
              MB_OK | MB_ICONWARNING);
}

void show_sidecar_repair_message(const HWND owner) {
  MessageBoxW(owner,
              L"Repair the invalid sidecar with Calibrate before "
              L"saving this profile. This prevents an unrelated base edit "
              L"from silently discarding recovery data.",
              L"Sidecar needs repair", MB_OK | MB_ICONWARNING);
}

} // namespace

bool confirm_schema_upgrade(const HWND owner) {
  return MessageBoxW(
             owner,
             L"Saving this legacy profile upgrades its base profile to schema version 4 and all declared module sidecars to their current schemas. Older LayerC builds cannot reopen the upgraded files. Continue?",
             L"Upgrade profile to LayerC V2.3", MB_YESNO | MB_ICONWARNING) ==
         IDYES;
}

void EditorWindow::sync_profile_from_controls() {
  if (shell_.syncing) return;
  auto *panel = panel_preview_.controls_panel < document_.profile().panels.size()
                    ? &document_.profile().panels[panel_preview_.controls_panel]
                    : nullptr;
  ProfileFieldContext context{shell_.window, document_.profile(), selected_cooldown(), panel};
  read_profile_fields(context);
  if (document_.visual()) document_.visual()->profile_id = document_.profile().id;
  if (document_.automation()) document_.automation()->profile_id = document_.profile().id;
}

bool EditorWindow::validate_profile() {
  if (!draft_controls_valid()) {
    show_invalid_field_message(shell_.window);
    return false;
  }
  sync_profile_from_controls();
  if (const auto error = normalize_for_commit()) {
    MessageBoxW(shell_.window, profile_error_text(*error).c_str(),
                L"Profile is incomplete", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

bool EditorWindow::confirm_v2_upgrade() {
  if ((!document_.legacy_base_schema() && !document_.legacy_visual_schema()) ||
      document_.upgrade_acknowledged())
    return true;
  if (!confirm_schema_upgrade(shell_.window))
    return false;
  document_.acknowledge_upgrade();
  return true;
}

bool EditorWindow::confirm_unsaved_changes() {
  if (!document_.dirty())
    return true;
  if (!document_.path().empty() && draft_controls_valid() && flush_autosave(false))
    return true;
  const int choice = MessageBoxW(
      shell_.window, L"The current profile has unsaved changes. Save them first?",
      L"Unsaved changes", MB_YESNOCANCEL | MB_ICONWARNING);
  return choice == IDYES ? save_profile(false) : choice == IDNO;
}

void EditorWindow::new_profile(const bool confirm) {
  if (confirm && !confirm_unsaved_changes())
    return;
  end_edit_on_window();
  document_.profile() = make_new_profile();
  document_.clear_sidecars();
  document_.clear_legacy_schemas();
  document_.path().clear();
  document_.set_dirty(true);
  reset_history();
  refresh_profile_controls();
  set_status(
      L"New profile ready — Save once to create it and enable autosave.");
}
void EditorWindow::reload_profile_list(const std::filesystem::path &select_path,
                                       const bool open_selection) {
  shell_.profiles.clear();
  int selected = -1;
  {
  auto sync = shell_.sync_scope();
  ListView_DeleteAllItems(shell_.profile_list);
  const auto directory = layerc::platform::profile_directory();
  const auto scan = layerc::platform::scan_profile_directory(directory);
  if (scan.error.empty()) {
    for (const auto &entry : scan.profiles)
      shell_.profiles.push_back({.path = entry.path,
                          .label = profile_list_label(entry.profile),
                          .enabled = entry.profile.enabled});
  }
  std::sort(shell_.profiles.begin(), shell_.profiles.end(),
            [](const ProfileEntry &left, const ProfileEntry &right) {
              return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
            });
  for (std::size_t index = 0; index < shell_.profiles.size(); ++index) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(index);
    item.pszText = const_cast<LPWSTR>(shell_.profiles[index].label.c_str());
    item.lParam = static_cast<LPARAM>(index);
    ListView_InsertItem(shell_.profile_list, &item);
    ListView_SetCheckState(shell_.profile_list, static_cast<int>(index),
                           shell_.profiles[index].enabled);
    if (!select_path.empty() && shell_.profiles[index].path.lexically_normal() ==
                                    select_path.lexically_normal())
      selected = static_cast<int>(index);
  }
  if (selected < 0 && !shell_.profiles.empty())
    selected = 0;
  if (selected >= 0)
    ListView_SetItemState(shell_.profile_list, selected, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
  if (open_selection && selected >= 0)
    open_profile_row(selected, false);
  else if (shell_.profiles.empty() && document_.path().empty())
    new_profile(false);
}

void EditorWindow::open_profile_row(const int row, const bool confirm) {
  if (row < 0 || static_cast<std::size_t>(row) >= shell_.profiles.size())
    return;
  const auto target_path = shell_.profiles[static_cast<std::size_t>(row)].path;
  const auto select_path = [&](const std::filesystem::path &path) {
    auto sync = shell_.sync_scope();
    ListView_SetItemState(shell_.profile_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (std::size_t index = 0; index < shell_.profiles.size(); ++index) {
      if (shell_.profiles[index].path.lexically_normal() != path.lexically_normal())
        continue;
      ListView_SetItemState(shell_.profile_list, static_cast<int>(index),
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(shell_.profile_list, static_cast<int>(index), FALSE);
      break;
    }
  };
  if (confirm && !confirm_unsaved_changes()) {
    select_path(document_.path());
    return;
  }

  auto loaded = layerc::platform::load_profile_bundle(target_path);
  if (auto *error = std::get_if<layerc::platform::ProfileBundleLoadError>(&loaded)) {
    select_path(document_.path());
    set_status(*error == layerc::platform::ProfileBundleLoadError::unreadable
                   ? L"Could not read the selected profile."
                   : L"The selected profile is invalid.");
    return;
  }
  document_.assign_bundle(
      target_path, std::get<layerc::platform::ProfileBundleLoad>(std::move(loaded)));
  refresh_visual_controls();
  document_.set_dirty(false);
  end_edit_on_window();
  reset_history();
  select_path(target_path);
  refresh_profile_controls();
  set_status(document_.profile().enabled ? L"Profile loaded."
                              : L"Disabled profile loaded.");
}

void EditorWindow::save_profile_enabled(const int row, const bool enabled) {
  if (row < 0 || static_cast<std::size_t>(row) >= shell_.profiles.size())
    return;
  auto &entry = shell_.profiles[static_cast<std::size_t>(row)];
  if (entry.enabled == enabled)
    return;
  const auto restore_checkbox = [&] {
    auto sync = shell_.sync_scope();
    ListView_SetCheckState(shell_.profile_list, row, entry.enabled);
  };
  const bool is_current =
      layerc::platform::canonical_profile_key(entry.path) ==
      layerc::platform::canonical_profile_key(document_.path());
  layerc::core::GameProfile stored_profile;
  std::optional<layerc::core::VisualProfile> stored_visual;
  std::optional<layerc::core::AutomationProfile> stored_automation;
  bool upgrade_needed = false;
  if (is_current) {
    if (!draft_controls_valid()) {
      restore_checkbox();
      set_status(L"Finish the invalid field before changing enablement.");
      return;
    }
    sync_profile_from_controls();
    if (normalize_for_commit().has_value()) {
      restore_checkbox();
      set_status(L"Finish the invalid field before changing enablement.");
      return;
    }
    if (!can_commit_document_bundle()) {
      restore_checkbox();
      set_status(
          L"Repair the invalid sidecar before changing enablement.");
      return;
    }
    stored_profile = document_.profile();
    stored_visual = document_.visual();
    stored_automation = document_.automation();
    upgrade_needed = document_needs_schema_upgrade(document_);
  } else {
    auto loaded = layerc::platform::load_profile_bundle(entry.path);
    const wchar_t *failure = nullptr;
    if (const auto *error =
            std::get_if<layerc::platform::ProfileBundleLoadError>(&loaded)) {
      failure = *error == layerc::platform::ProfileBundleLoadError::unreadable
                    ? L"Could not update the selected profile."
                    : L"Could not update an invalid profile.";
    } else {
      auto &bundle = std::get<layerc::platform::ProfileBundleLoad>(loaded);
      if (bundle.visual.state == layerc::platform::ModuleSidecarState::invalid)
        failure = L"Could not update a profile with an invalid visual sidecar.";
      else if (bundle.automation.state ==
               layerc::platform::ModuleSidecarState::invalid)
        failure = L"Could not update a profile with an invalid automation sidecar.";
      stored_profile = std::move(bundle.profile);
      stored_visual = std::move(bundle.visual.profile);
      stored_automation = std::move(bundle.automation.profile);
      upgrade_needed = bundle.legacy_base_schema || bundle.legacy_visual_schema;
    }
    if (failure != nullptr) {
      restore_checkbox();
      set_status(failure);
      return;
    }
  }
  stored_profile.enabled = enabled;
  switch (commit_bundle(entry.path, stored_profile, stored_visual,
                        stored_automation, upgrade_needed)) {
  case CommitResult::ok:
    break;
  case CommitResult::upgrade_declined:
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    restore_checkbox();
    set_status(L"Profile enablement was not changed.");
    return;
  case CommitResult::blocked_by_sidecar:
    restore_checkbox();
    set_status(L"Could not save the profile enablement change.");
    return;
  case CommitResult::write_failed:
    restore_checkbox();
    set_status(write_failure_message());
    return;
  }
  entry.enabled = enabled;
  entry.label = profile_list_label(stored_profile);
  {
    auto sync = shell_.sync_scope();
    ListView_SetItemText(shell_.profile_list, row, 0, entry.label.data());
  }
  if (is_current) {
    document_.visual() = stored_visual;
    document_.automation() = stored_automation;
    document_.mark_visual_edited();
    document_.mark_automation_edited();
    const bool enablement_changes = document_.profile() != stored_profile;
    if (enablement_changes) {
      (void)document_.history().record(stored_profile, document_.visual(),
                            document_.automation(), false);
    }
    document_.profile() = stored_profile;
    mark_committed();
  } else {
    mark_committed(entry.path, stored_profile);
  }
  set_status(enabled ? L"Profile enabled — LayerD updated live."
                     : L"Profile disabled — LayerD retired its instances.");
  InvalidateRect(shell_.profile_list, nullptr, TRUE);
}

bool EditorWindow::choose_save_path(std::filesystem::path &path) {
  const auto directory = layerc::platform::profile_directory();
  if (directory.empty()) {
    MessageBoxW(shell_.window, L"The profiles directory is unavailable.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    MessageBoxW(shell_.window, L"Could not create the profiles directory.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }

  const auto initial_filename = path.empty()
                                    ? default_profile_filename(document_.profile())
                                    : path.filename();
  auto initial_path = directory / initial_filename;
  const auto initial_directory = directory.wstring();
  for (;;) {
    std::vector<wchar_t> buffer(32'768);
    wcsncpy_s(buffer.data(), buffer.size(), initial_path.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = this->shell_.window;
    dialog.lpstrFilter = L"LayerC profiles (*.json)\0*.json\0All files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = initial_directory.c_str();
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog) == FALSE)
      return false;

    auto candidate = std::filesystem::path(buffer.data());
    if (candidate.extension().empty())
      candidate += L".json";
    if (_wcsicmp(candidate.extension().c_str(), L".json") != 0) {
      candidate.replace_extension(L".json");
      MessageBoxW(shell_.window, L"LayerC profiles must use the .json extension.",
                  L"Choose a JSON profile name", MB_OK | MB_ICONWARNING);
      initial_path = directory / candidate.filename();
      continue;
    }
    std::error_code directory_error;
    std::error_code parent_error;
    const auto canonical_directory =
        std::filesystem::weakly_canonical(directory, directory_error);
    const auto canonical_parent = std::filesystem::weakly_canonical(
        candidate.parent_path(), parent_error);
    if (!directory_error && !parent_error &&
        _wcsicmp(canonical_directory.c_str(), canonical_parent.c_str()) == 0) {
      path = std::move(candidate);
      return true;
    }

    MessageBoxW(shell_.window,
                L"Profiles must be saved directly in the LayerC profiles "
                L"folder so LayerD can load them.",
                L"Choose the profiles folder", MB_OK | MB_ICONWARNING);
    initial_path = directory / candidate.filename();
  }
}

bool EditorWindow::save_profile(const bool force_save_as) {
  if (!validate_profile())
    return false;
  // Shared document-commit gate: every UI entry point blocks on invalid
  // modules so unrelated edits never discard recovery data.
  if (!can_commit_document_bundle()) {
    show_sidecar_repair_message(shell_.window);
    return false;
  }
  auto destination = document_.path();
  if (force_save_as) {
    if (!choose_save_path(destination))
      return false;
  } else if (destination.empty()) {
    destination = layerc::platform::profile_directory() /
                  default_profile_filename(document_.profile());
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        MessageBoxW(
            shell_.window, L"A profile with this ID already exists. Overwrite it?",
            L"Profile already exists", MB_YESNO | MB_ICONWARNING) != IDYES)
      return false;
    if (error) {
      MessageBoxW(shell_.window, L"Could not inspect the profile destination.",
                  L"LayerC Editor", MB_OK | MB_ICONERROR);
      return false;
    }
  }
  switch (commit_bundle(destination)) {
  case CommitResult::ok:
    break;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    show_invalid_field_message(shell_.window);
    return false;
  case CommitResult::upgrade_declined:
    return false;
  case CommitResult::blocked_by_sidecar:
    show_sidecar_repair_message(shell_.window);
    return false;
  case CommitResult::write_failed:
    MessageBoxW(shell_.window, write_failure_message().c_str(),
                L"Profile save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  document_.path() = destination;
  mark_committed(destination, document_.profile(), {.refresh_list = true});
  set_status(L"Profile saved — LayerD updated live.");
  return true;
}

void EditorWindow::delete_profile() {
  if (document_.path().empty()) {
    MessageBoxW(shell_.window, L"Save this profile before deleting it.",
                L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!confirm_unsaved_changes())
    return;
  const auto prompt =
      L"Move '" + to_wide(document_.profile().name) + L"' to the Recycle Bin?";
  if (MessageBoxW(shell_.window, prompt.c_str(), L"Delete profile",
                  MB_YESNO | MB_ICONWARNING) != IDYES)
    return;
  std::filesystem::path safe_remaining;
  for (const auto &entry : shell_.profiles) {
    if (entry.path.lexically_normal() != document_.path().lexically_normal()) {
      safe_remaining = entry.path;
      break;
    }
  }
  if (!recycle_profile_bundle(document_.path())) {
    MessageBoxW(shell_.window, L"Could not move the profile to the Recycle Bin.",
                L"LayerC Editor", MB_OK | MB_ICONERROR);
    return;
  }
  end_edit_on_window();
  document_.path().clear();
  document_.set_dirty(false);
  document_.clear_sidecars();
  (void)layerc::platform::signal_profile_reload();
  reload_profile_list(safe_remaining, true);
  set_status(safe_remaining.empty() ? L"Profile moved to the Recycle Bin."
                                    : L"Profile deleted and LayerD reloaded.");
  report_visual_asset_gc(
      layerc::platform::garbage_collect_unreferenced_visual_assets());
}

void EditorWindow::open_profiles_folder() {
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || directory.empty()) {
    MessageBoxW(shell_.window, L"Could not open the profiles folder.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return;
  }
  ShellExecuteW(shell_.window, L"open", directory.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

bool EditorWindow::can_commit_document_bundle() const {
  // Invalid (unreadable) modules have no usable parsed profile; committing
  // with a nullopt would plan an explicit removal and discard recovery
  // bytes/backups. Absent modules are a valid keep-absent intent.
  if (!document_.visual() &&
      document_.visual_state() == layerc::platform::ModuleSidecarState::invalid)
    return false;
  if (!document_.automation() &&
      document_.automation_state() ==
          layerc::platform::ModuleSidecarState::invalid)
    return false;
  return true;
}

layerc::platform::ProfileWriteResult EditorWindow::commit_document_bundle(
    const std::filesystem::path &destination) {
  if (destination.empty() || !can_commit_document_bundle())
    return {layerc::platform::ProfileWriteError::invalid_input};
  return layerc::platform::write_profile_bundle(
      destination, document_.profile(), document_.visual(), document_.automation());
}

std::wstring EditorWindow::write_failure_message() const {
  return L"Profile write failed: " +
         to_wide(layerc::platform::profile_write_error_text(
             last_write_error_.value_or(layerc::platform::ProfileWriteError::io_failed)));
}

namespace {

std::optional<layerc::core::ProfileError> normalize_profile_bundle(
    layerc::core::GameProfile &profile,
    std::optional<layerc::core::VisualProfile> &visual,
    std::optional<layerc::core::AutomationProfile> &automation) {
  const auto parsed = layerc::core::parse_profile_json(
      layerc::core::serialize_profile_json(profile));
  if (const auto *error = std::get_if<layerc::core::ProfileError>(&parsed))
    return *error;
  profile = std::get<layerc::core::GameProfile>(parsed);
  if (visual) visual->profile_id = profile.id;
  if (automation) automation->profile_id = profile.id;
  return std::nullopt;
}

} // namespace

std::optional<layerc::core::ProfileError> EditorWindow::normalize_for_commit() {
  return normalize_profile_bundle(document_.profile(), document_.visual(),
                                  document_.automation());
}

CommitResult EditorWindow::commit_bundle(const std::filesystem::path &path,
                                         const CommitOptions &options) {
  if (!draft_controls_valid()) return CommitResult::invalid_fields;
  sync_profile_from_controls();
  if (normalize_for_commit().has_value()) return CommitResult::invalid_profile;
  if (document_needs_schema_upgrade(document_)) {
    if (!options.prompt_upgrade || !confirm_v2_upgrade())
      return CommitResult::upgrade_declined;
  }
  if (!can_commit_document_bundle()) return CommitResult::blocked_by_sidecar;
  const auto write = commit_document_bundle(path);
  last_write_error_ = write.error();
  if (!write) return CommitResult::write_failed;
  return CommitResult::ok;
}

CommitResult EditorWindow::commit_bundle(
    const std::filesystem::path &path, layerc::core::GameProfile &profile,
    std::optional<layerc::core::VisualProfile> &visual,
    std::optional<layerc::core::AutomationProfile> &automation,
    const bool upgrade_needed, const CommitOptions &options) {
  if (upgrade_needed) {
    if (!options.prompt_upgrade) return CommitResult::upgrade_declined;
    if (!(is_document_path(document_, path) ? confirm_v2_upgrade()
                              : confirm_schema_upgrade(shell_.window)))
      return CommitResult::upgrade_declined;
  }
  if (normalize_profile_bundle(profile, visual, automation).has_value())
    return CommitResult::invalid_profile;
  const auto write = path.empty()
                         ? layerc::platform::ProfileWriteResult{
                               layerc::platform::ProfileWriteError::invalid_input}
                         : layerc::platform::write_profile_bundle(
                               path, profile, visual, automation);
  last_write_error_ = write.error();
  if (!write) return CommitResult::write_failed;
  return CommitResult::ok;
}

void EditorWindow::mark_committed(const CommitOptions &options) {
  mark_committed(document_.path(), document_.profile(), options);
}

void EditorWindow::mark_committed(const std::filesystem::path &path,
                                  const layerc::core::GameProfile &profile,
                                  const CommitOptions &options) {
  if (is_document_path(document_, path)) {
    document_.set_dirty(false);
    document_.clear_legacy_schemas();
    document_.history().rebase(document_.profile(), document_.visual(),
                               document_.automation());
    KillTimer(shell_.window, autosave_timer);
    if (options.refresh_list)
      reload_profile_list(path, false);
    else
      sync_current_profile_entry();
  }
  notify_profile_committed(path, profile);
}

} // namespace layerc::editor
