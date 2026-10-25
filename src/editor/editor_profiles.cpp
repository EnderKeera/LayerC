#include "document_commit.hpp"
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

void show_invalid_field_message(const HWND owner) {
  const auto limit = std::chrono::duration_cast<std::chrono::seconds>(
      layerc::core::max_cooldown_duration);
  const std::wstring message =
      L"Finish the invalid field. Cooldown duration must be greater than 0 "
      L"and no more than " +
      std::to_wstring(limit.count()) + L" seconds.";
  themed_message_box(owner, message.c_str(), L"Profile is incomplete",
              MB_OK | MB_ICONWARNING);
}

void show_sidecar_repair_message(const HWND owner) {
  themed_message_box(owner,
              L"Repair the invalid visual settings with Calibrate before "
              L"saving this profile. This prevents an unrelated base edit "
              L"from silently discarding recovery data.",
              L"Settings need repair", MB_OK | MB_ICONWARNING);
}

} // namespace

void ProfileEntry::describe(const layerc::core::GameProfile &profile) {
  label = profile_list_label(profile);
  name = to_wide(profile.name);
  executable = to_wide(profile.game.executable_name);
  enabled = profile.enabled;
}

bool confirm_schema_upgrade(const HWND owner) {
  return themed_message_box(
             owner,
             L"This profile was saved by an older LayerC. Saving upgrades it, with its visual and automation settings, to the current profile format. Older LayerC versions cannot open it afterwards. Continue?",
             L"Upgrade profile format", MB_YESNO | MB_ICONWARNING) ==
         IDYES;
}

bool EditorWindow::validate_profile() {
  if (!draft_controls_valid()) {
    show_invalid_field_message(shell_.window);
    return false;
  }
  if (!apply_profile_controls(DocumentChangeKind::text)) return false;
  if (const auto error = normalize_for_commit()) {
    themed_message_box(shell_.window, profile_error_text(*error).c_str(),
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
  if (!has_unsaved_changes())
    return true;
  if (!document_.path().empty() && draft_controls_valid() && flush_autosave(false))
    return true;
  const int choice = themed_message_box(
      shell_.window, L"The current profile has unsaved changes. Save them first?",
      L"Unsaved changes", MB_YESNOCANCEL | MB_ICONWARNING);
  return choice == IDYES ? save_profile(false) : choice == IDNO;
}

void EditorWindow::new_profile(const bool confirm) {
  if (confirm && !confirm_unsaved_changes())
    return;
  end_edit_on_window();
  // Untouched drafts are clean: the first real edit marks the document dirty,
  // so leaving an unedited New Profile never asks to save it.
  document_.start_draft(make_new_profile(), DraftOrigin::fresh);
  refresh_profile_controls();
  visual_editor_.refresh();
  sync_draft_row();
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
    for (const auto &entry : scan.profiles) {
      auto &added = shell_.profiles.emplace_back();
      added.path = entry.path;
      added.describe(entry.profile);
    }
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
  else
    sync_draft_row();
}

void EditorWindow::open_profile_row(const int row, const bool confirm) {
  if (row < 0 || static_cast<std::size_t>(row) >= shell_.profiles.size())
    return;
  const auto target_path = shell_.profiles[static_cast<std::size_t>(row)].path;
  // The provisional draft row is the open document already.
  if (target_path.empty())
    return;
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
  document_.open(target_path,
                 std::get<layerc::platform::ProfileBundleLoad>(std::move(loaded)));
  visual_editor_.refresh();
  end_edit_on_window();
  sync_draft_row();
  select_path(target_path);
  refresh_profile_controls();
  set_status(document_.profile().enabled ? L"Profile loaded."
                              : L"Profile loaded (off).");
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
  if (entry.path.empty()) {
    // The unsaved draft has no file to turn on or off.
    restore_checkbox();
    return;
  }
  const bool is_current =
      layerc::platform::canonical_profile_key(entry.path) ==
      layerc::platform::canonical_profile_key(document_.path());
  if (is_current) {
    if (!draft_controls_valid()) {
      restore_checkbox();
      set_status(L"Finish the invalid field before turning the profile on or off.");
      return;
    }
    if (!apply_profile_controls(DocumentChangeKind::text)) {
      restore_checkbox();
      set_status(L"Finish the invalid field before turning the profile on or off.");
      return;
    }
  }
  auto selected = profile_enablement_bundle(
      document_, is_current, entry.path, OpenDocumentEnablement::commit_open_edits);
  if (const auto *refusal = std::get_if<EnablementRefusal>(&selected)) {
    restore_checkbox();
    set_status(*refusal == EnablementRefusal::unreadable
                   ? L"Could not update the selected profile."
                   : L"Could not update an invalid profile.");
    return;
  }
  auto bundle = std::get<ProfileBundle>(std::move(selected));
  bundle.profile.enabled = enabled;
  switch (commit_bundle(entry.path, bundle)) {
  case CommitResult::ok:
    break;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    restore_checkbox();
    set_status(is_current
                   ? L"Finish the invalid field before turning the profile on or off."
                   : L"Profile enablement was not changed.");
    return;
  case CommitResult::upgrade_declined:
    restore_checkbox();
    set_status(L"Profile enablement was not changed.");
    return;
  case CommitResult::blocked_by_sidecar:
    restore_checkbox();
    set_status(L"Repair the invalid visual or automation settings before "
               L"turning the profile on or off.");
    return;
  case CommitResult::write_failed:
    restore_checkbox();
    set_status(write_failure_message());
    return;
  }
  entry.describe(bundle.profile);
  {
    auto sync = shell_.sync_scope();
    ListView_SetItemText(shell_.profile_list, row, 0, entry.label.data());
  }
  if (is_current) {
    const auto committed_path = entry.path;
    document_.accept_commit(committed_path, std::move(bundle), CommitRecord::record_edit);
    mark_committed();
  } else {
    mark_committed(entry.path, bundle.profile);
  }
  set_status(enabled ? L"Profile enabled — LayerD updated live."
                     : L"Profile disabled — LayerD retired its instances.");
  InvalidateRect(shell_.profile_list, nullptr, TRUE);
}

bool EditorWindow::choose_save_path(std::filesystem::path &path) {
  const auto directory = layerc::platform::profile_directory();
  if (directory.empty()) {
    themed_message_box(shell_.window, L"The profiles directory is unavailable.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    themed_message_box(shell_.window, L"Could not create the profiles directory.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }

  auto initial_path = path.empty()
                          ? default_profile_path(directory, document_.profile())
                          : directory / path.filename();
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
      themed_message_box(shell_.window, L"LayerC profiles must use the .json extension.",
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

    themed_message_box(shell_.window,
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
  if (document_.has_unreadable_module()) {
    show_sidecar_repair_message(shell_.window);
    return false;
  }
  auto destination = document_.path();
  if (force_save_as) {
    if (!choose_save_path(destination))
      return false;
  } else if (destination.empty()) {
    destination = default_profile_path(layerc::platform::profile_directory(),
                                       document_.profile());
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        themed_message_box(
            shell_.window, L"A profile with this ID already exists. Overwrite it?",
            L"Profile already exists", MB_YESNO | MB_ICONWARNING) != IDYES)
      return false;
    if (error) {
      themed_message_box(shell_.window, L"Could not inspect the profile destination.",
                  L"LayerC Editor", MB_OK | MB_ICONERROR);
      return false;
    }
  }
  switch (commit_document(destination)) {
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
    themed_message_box(shell_.window, write_failure_message().c_str(),
                L"Profile save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  mark_committed(destination, document_.profile(), {.refresh_list = true});
  set_status(L"Profile saved — LayerD updated live.");
  return true;
}

void EditorWindow::delete_profile() {
  if (document_.path().empty()) {
    // Deleting the unsaved draft discards it.
    if (!confirm_unsaved_changes())
      return;
    const auto saved = std::ranges::find_if(
        shell_.profiles, [](const ProfileEntry &entry) {
          return !entry.path.empty();
        });
    if (saved == shell_.profiles.end()) {
      new_profile(false);
    } else {
      open_profile_row(static_cast<int>(saved - shell_.profiles.begin()), false);
    }
    set_status(L"Draft discarded.");
    return;
  }
  if (!confirm_unsaved_changes())
    return;
  const auto prompt =
      L"Move '" + to_wide(document_.profile().name) + L"' to the Recycle Bin?";
  if (themed_message_box(shell_.window, prompt.c_str(), L"Delete profile",
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
    themed_message_box(shell_.window, L"Could not move the profile to the Recycle Bin.",
                L"LayerC Editor", MB_OK | MB_ICONERROR);
    return;
  }
  end_edit_on_window();
  // Same clean, module-less detached state as before, minus stale undo history.
  document_.start_draft(document_.profile(), DraftOrigin::fresh);
  (void)layerc::platform::signal_profile_reload();
  reload_profile_list(safe_remaining, true);
  set_status(safe_remaining.empty() ? L"Profile moved to the Recycle Bin."
                                    : L"Profile deleted and LayerD reloaded.");
}

void EditorWindow::open_profiles_folder() {
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || directory.empty()) {
    themed_message_box(shell_.window, L"Could not open the profiles folder.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return;
  }
  ShellExecuteW(shell_.window, L"open", directory.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

std::wstring EditorWindow::write_failure_message() const {
  return to_wide(write_failure_text(last_write_error_));
}

std::optional<layerc::core::ProfileError> EditorWindow::normalize_for_commit() {
  // Validate a detached copy: the document changes only through a commit.
  auto bundle = document_.bundle();
  return normalize_profile_bundle(bundle.profile, bundle.visual,
                                  bundle.automation);
}

CommitResult EditorWindow::commit_bundle(const std::filesystem::path &path,
                                         ProfileBundle &bundle,
                                         const CommitOptions &options) {
  const auto outcome = commit_profile_bundle(path, bundle, options, [&] {
    return is_document_path(document_, path)
               ? confirm_v2_upgrade()
               : confirm_schema_upgrade(shell_.window);
  });
  last_write_error_ = outcome.write_error;
  return outcome.result;
}

CommitResult EditorWindow::commit_document(
    const std::filesystem::path &path, const CommitOptions &options,
    const std::function<void(ProfileBundle &)> &candidate) {
  if (!draft_controls_valid()) return CommitResult::invalid_fields;
  if (!apply_profile_controls(DocumentChangeKind::text))
    return CommitResult::invalid_fields;
  const auto outcome = layerc::editor::commit_document(
      document_, path, options, [&] {
        return is_document_path(document_, path)
                   ? confirm_v2_upgrade()
                   : confirm_schema_upgrade(shell_.window);
      }, layerc::platform::KeepModule{}, {}, candidate);
  last_write_error_ = outcome.write_error;
  return outcome.result;
}

void EditorWindow::mark_committed(const CommitOptions &options) {
  mark_committed(document_.path(), document_.profile(), options);
}

void EditorWindow::mark_committed(const std::filesystem::path &path,
                                  const layerc::core::GameProfile &profile,
                                  const CommitOptions &options) {
  if (is_document_path(document_, path)) {
    // The commit already installed the saved transition.
    KillTimer(shell_.window, autosave_timer);
    if (options.refresh_list)
      reload_profile_list(path, false);
    else
      sync_current_profile_entry();
    refresh_title();
  }
  notify_profile_committed(path, profile);
}

void EditorWindow::refresh_profile_controls() {
  cooldown_editor_.reset_capture();
  auto sync = shell_.sync_scope();
  const ProfileFieldView context{shell_.window, document_.profile()};
  write_profile_fields(context, ProfileFieldOwner::profile);
  set_owner_draw_checkbox(
      GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH),
      !document_.profile().game.executable_path.empty());
  // An empty field shows its cue banner rather than hint text as a value.
  const auto &game = document_.profile().game;
  set_text(shell_.application_path,
           to_wide(game.executable_path.empty() ? game.executable_name
                                                : game.executable_path));
  // Keep the executable name (the end of a path) in view; the tooltip has
  // the whole path.
  const auto length = GetWindowTextLengthW(shell_.application_path);
  SendMessageW(shell_.application_path, EM_SETSEL, static_cast<WPARAM>(length),
               length);
  SendMessageW(shell_.application_path, EM_SCROLLCARET, 0, 0);
  cooldown_editor_.refresh_list();
  refresh_bar_list();
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
  layout();
  refresh_title();
}

} // namespace layerc::editor
