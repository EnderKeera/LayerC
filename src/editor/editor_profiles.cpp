#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/visual_assets.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <utility>
#include <variant>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windows.h>
namespace layerc::editor {
void EditorWindow::sync_profile_from_controls() {
  if (syncing_)
    return;
  profile_.name = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_NAME)));
  profile_.id = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_ID)));
  if (auto *cooldown = selected_cooldown(); cooldown != nullptr) {
    cooldown->name =
        to_utf8(control_text(GetDlgItem(dialog_, IDC_COOLDOWN_NAME)));
    if (const auto duration = parse_duration_ms(
            control_text(GetDlgItem(dialog_, IDC_COOLDOWN_DURATION))))
      cooldown->duration = *duration;
    cooldown->max_charges = parse_uint(
        control_text(GetDlgItem(dialog_, IDC_COOLDOWN_CHARGES)),
        cooldown->max_charges);
    const auto retrigger = SendMessageW(
        GetDlgItem(dialog_, IDC_COOLDOWN_RETRIGGER), CB_GETCURSEL, 0, 0);
    if (retrigger >= 0 && retrigger <= 1)
      cooldown->retrigger =
          static_cast<layerc::core::RetriggerPolicy>(retrigger);
    const auto recovery = SendMessageW(
        GetDlgItem(dialog_, IDC_COOLDOWN_RECOVERY), CB_GETCURSEL, 0, 0);
    if (recovery >= 0 && recovery <= 2)
      cooldown->recovery = static_cast<layerc::core::RecoveryMode>(recovery);
  }
  if (visual_profile_)
    visual_profile_->profile_id = profile_.id;
  if (automation_profile_)
    automation_profile_->profile_id = profile_.id;
  if (bar_controls_panel_ < profile_.panels.size()) {
    auto &panel = profile_.panels[bar_controls_panel_];
    panel.bar.width = parse_uint(
        control_text(GetDlgItem(dialog_, IDC_BAR_WIDTH)), panel.bar.width);
    panel.bar.height = parse_uint(
        control_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT)), panel.bar.height);
    panel.bar.show_text =
        owner_draw_checkbox_checked(GetDlgItem(dialog_, IDC_SHOW_TEXT));
    panel.bar.hide_when_ready =
        owner_draw_checkbox_checked(GetDlgItem(dialog_, IDC_HIDE_READY));
  }
}

bool EditorWindow::validate_profile() {
  if (!draft_controls_valid()) {
    MessageBoxW(dialog_,
                L"Finish the invalid field. Cooldown duration must be greater "
                L"than 0 and no more than 86400 seconds.",
                L"Profile is incomplete", MB_OK | MB_ICONWARNING);
    return false;
  }
  sync_profile_from_controls();
  const auto parsed = layerc::core::parse_profile_json(
      layerc::core::serialize_profile_json(profile_));
  if (const auto *error = std::get_if<layerc::core::ProfileError>(&parsed)) {
    MessageBoxW(dialog_, profile_error_text(*error).c_str(),
                L"Profile is incomplete", MB_OK | MB_ICONWARNING);
    return false;
  }
  profile_ = std::get<layerc::core::GameProfile>(parsed);
  return true;
}

bool EditorWindow::confirm_v2_upgrade() {
  if ((!legacy_base_schema_ && !legacy_visual_schema_) ||
      v2_upgrade_acknowledged_)
    return true;
  if (!confirm_schema_upgrade(dialog_))
    return false;
  v2_upgrade_acknowledged_ = true;
  return true;
}

bool EditorWindow::confirm_unsaved_changes() {
  if (!dirty_)
    return true;
  if (!current_path_.empty() && draft_controls_valid() && flush_autosave(false))
    return true;
  const int choice = MessageBoxW(
      dialog_, L"The current profile has unsaved changes. Save them first?",
      L"Unsaved changes", MB_YESNOCANCEL | MB_ICONWARNING);
  return choice == IDYES ? save_profile(false) : choice == IDNO;
}

void EditorWindow::new_profile(const bool confirm) {
  if (confirm && !confirm_unsaved_changes())
    return;
  end_edit_on_window();
  profile_ = make_new_profile();
  visual_profile_.reset();
  automation_profile_.reset();
  automation_sidecar_state_ = layerc::platform::AutomationSidecarState::absent;
  automation_sidecar_error_.clear();
  visual_sidecar_state_ = layerc::platform::VisualSidecarState::absent;
  visual_sidecar_error_.clear();
  legacy_base_schema_ = false;
  legacy_visual_schema_ = false;
  v2_upgrade_acknowledged_ = false;
  current_path_.clear();
  dirty_ = true;
  reset_history();
  refresh_profile_controls();
  set_status(
      L"New profile ready — Save once to create it and enable autosave.");
}
void EditorWindow::reload_profile_list(const std::filesystem::path &select_path,
                                       const bool open_selection) {
  entries_.clear();
  syncing_ = true;
  ListView_DeleteAllItems(profile_list_);
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  if (!directory.empty() && std::filesystem::exists(directory, error) &&
      !error) {
    for (const auto &entry :
         std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() ||
          _wcsicmp(entry.path().extension().c_str(), L".json") != 0)
        continue;
      const auto contents = layerc::platform::read_profile_file(entry.path());
      if (!contents.has_value())
        continue;
      const auto parsed = layerc::core::parse_profile_json(*contents);
      const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
      if (profile == nullptr)
        continue;
      entries_.push_back({.path = entry.path(),
                          .label = profile_list_label(*profile),
                          .enabled = profile->enabled});
    }
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const ProfileEntry &left, const ProfileEntry &right) {
              return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
            });
  int selected = -1;
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(index);
    item.pszText = const_cast<LPWSTR>(entries_[index].label.c_str());
    item.lParam = static_cast<LPARAM>(index);
    ListView_InsertItem(profile_list_, &item);
    ListView_SetCheckState(profile_list_, static_cast<int>(index),
                           entries_[index].enabled);
    if (!select_path.empty() && entries_[index].path.lexically_normal() ==
                                    select_path.lexically_normal())
      selected = static_cast<int>(index);
  }
  if (selected < 0 && !entries_.empty())
    selected = 0;
  if (selected >= 0)
    ListView_SetItemState(profile_list_, selected, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  syncing_ = false;
  if (open_selection && selected >= 0)
    open_profile_row(selected, false);
  else if (entries_.empty() && current_path_.empty())
    new_profile(false);
}

void EditorWindow::open_profile_row(const int row, const bool confirm) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size())
    return;
  const auto target_path = entries_[static_cast<std::size_t>(row)].path;
  const auto select_path = [&](const std::filesystem::path &path) {
    syncing_ = true;
    ListView_SetItemState(profile_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].path.lexically_normal() != path.lexically_normal())
        continue;
      ListView_SetItemState(profile_list_, static_cast<int>(index),
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(profile_list_, static_cast<int>(index), FALSE);
      break;
    }
    syncing_ = false;
  };
  if (confirm && !confirm_unsaved_changes()) {
    select_path(current_path_);
    return;
  }

  const auto contents = layerc::platform::read_profile_file(target_path);
  if (!contents.has_value()) {
    select_path(current_path_);
    set_status(L"Could not read the selected profile.");
    return;
  }
  const auto parsed = layerc::core::parse_profile_json(*contents);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (profile == nullptr) {
    select_path(current_path_);
    set_status(L"The selected profile is invalid.");
    return;
  }
  profile_ = *profile;
  legacy_base_schema_ = source_uses_legacy_base_schema(*contents);
  v2_upgrade_acknowledged_ = false;
  current_path_ = target_path;
  load_current_visual_profile();
  load_current_automation_profile();
  dirty_ = false;
  end_edit_on_window();
  reset_history();
  select_path(target_path);
  refresh_profile_controls();
  set_status(profile_.enabled ? L"Profile loaded."
                              : L"Disabled profile loaded.");
}

void EditorWindow::save_profile_enabled(const int row, const bool enabled) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size())
    return;
  auto &entry = entries_[static_cast<std::size_t>(row)];
  if (entry.enabled == enabled)
    return;
  const auto restore_checkbox = [&] {
    const bool was_syncing = syncing_;
    syncing_ = true;
    ListView_SetCheckState(profile_list_, row, entry.enabled);
    syncing_ = was_syncing;
  };
  const bool is_current =
      layerc::platform::canonical_profile_key(entry.path) ==
      layerc::platform::canonical_profile_key(current_path_);
  layerc::core::GameProfile stored_profile;
  std::optional<layerc::core::VisualProfile> stored_visual;
  std::optional<layerc::core::AutomationProfile> stored_automation;
  bool legacy_upgrade{};
  bool commit_bundle{is_current};
  if (is_current) {
    sync_profile_from_controls();
    if (!draft_controls_valid()) {
      restore_checkbox();
      set_status(L"Finish the invalid field before changing enablement.");
      return;
    }
    stored_profile = profile_;
    if (visual_sidecar_state_ ==
        layerc::platform::VisualSidecarState::invalid) {
      restore_checkbox();
      set_status(
          L"Repair the invalid visual sidecar before changing enablement.");
      return;
    }
    if (automation_sidecar_state_ ==
        layerc::platform::AutomationSidecarState::invalid) {
      restore_checkbox();
      set_status(
          L"Repair the invalid automation sidecar before changing enablement.");
      return;
    }
    stored_visual = visual_profile_;
    stored_automation = automation_profile_;
    legacy_upgrade = legacy_base_schema_ || legacy_visual_schema_;
  } else {
    const auto contents = layerc::platform::read_profile_file(entry.path);
    if (!contents.has_value()) {
      restore_checkbox();
      set_status(L"Could not update the selected profile.");
      return;
    }
    auto parsed = layerc::core::parse_profile_json(*contents);
    const auto *stored = std::get_if<layerc::core::GameProfile>(&parsed);
    if (stored == nullptr) {
      restore_checkbox();
      set_status(L"Could not update an invalid profile.");
      return;
    }
    stored_profile = *stored;
    const auto visual =
        layerc::platform::load_visual_sidecar(entry.path, stored_profile);
    if (visual.state == layerc::platform::VisualSidecarState::invalid) {
      restore_checkbox();
      set_status(L"Could not update a profile with an invalid visual sidecar.");
      return;
    }
    stored_visual = visual.profile;
    const auto automation =
        layerc::platform::load_automation_sidecar(entry.path, stored_profile);
    if (automation.state == layerc::platform::AutomationSidecarState::invalid) {
      restore_checkbox();
      set_status(
          L"Could not update a profile with an invalid automation sidecar.");
      return;
    }
    stored_automation = automation.profile;
    const bool legacy_base = source_uses_legacy_base_schema(*contents);
    legacy_upgrade = legacy_base;
    commit_bundle = commit_bundle || legacy_base;
    if (!visual.source_path.empty()) {
      const auto visual_contents =
          layerc::platform::read_visual_file(visual.source_path);
      const bool legacy_visual =
          visual_contents.has_value() &&
          source_uses_legacy_visual_schema(*visual_contents);
      legacy_upgrade = legacy_upgrade || legacy_visual;
      commit_bundle = commit_bundle || legacy_visual;
    }
  }
  stored_profile.enabled = enabled;
  if (legacy_upgrade &&
      !(is_current ? confirm_v2_upgrade() : confirm_schema_upgrade(dialog_))) {
    restore_checkbox();
    set_status(L"Profile enablement was not changed.");
    return;
  }
  const auto serialized = layerc::core::serialize_profile_json(stored_profile);
  const bool serialized_valid =
      std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(serialized));
  const bool persisted =
      serialized_valid &&
      (commit_bundle
           ? layerc::platform::write_profile_bundle(
                 entry.path, stored_profile, stored_visual, stored_automation)
           : layerc::platform::write_profile_file(entry.path, serialized));
  if (!persisted) {
    restore_checkbox();
    set_status(L"Could not save the profile enablement change.");
    return;
  }
  entry.enabled = enabled;
  entry.label = profile_list_label(stored_profile);
  const bool was_syncing = syncing_;
  syncing_ = true;
  ListView_SetItemText(profile_list_, row, 0, entry.label.data());
  syncing_ = was_syncing;
  if (is_current) {
    visual_profile_ = stored_visual;
    automation_profile_ = stored_automation;
    if (automation_profile_) {
      automation_sidecar_state_ =
          layerc::platform::AutomationSidecarState::valid;
      automation_sidecar_error_.clear();
    }
    if (visual_profile_) {
      visual_sidecar_state_ = layerc::platform::VisualSidecarState::valid;
      visual_sidecar_error_.clear();
    }
    if (profile_ != stored_profile) {
      (void)history_.record(stored_profile, visual_profile_,
                            automation_profile_, false);
    }
    profile_ = stored_profile;
    history_.rebase(profile_, visual_profile_, automation_profile_);
    dirty_ = false;
    legacy_base_schema_ = false;
    legacy_visual_schema_ = false;
    v2_upgrade_acknowledged_ = false;
    KillTimer(dialog_, autosave_timer);
  }
  notify_profile_committed(entry.path, stored_profile);
  set_status(enabled ? L"Profile enabled — LayerD updated live."
                     : L"Profile disabled — LayerD retired its instances.");
  InvalidateRect(profile_list_, nullptr, TRUE);
}

bool EditorWindow::choose_save_path(std::filesystem::path &path) {
  const auto directory = layerc::platform::profile_directory();
  if (directory.empty()) {
    MessageBoxW(dialog_, L"The profiles directory is unavailable.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    MessageBoxW(dialog_, L"Could not create the profiles directory.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return false;
  }

  const auto initial_filename =
      path.empty()
          ? std::filesystem::path(
                to_wide(safe_identifier(to_wide(profile_.id))) + L".json")
          : path.filename();
  auto initial_path = directory / initial_filename;
  const auto initial_directory = directory.wstring();
  for (;;) {
    std::array<wchar_t, 32'768> buffer{};
    wcsncpy_s(buffer.data(), buffer.size(), initial_path.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = this->dialog_;
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
      MessageBoxW(dialog_, L"LayerC profiles must use the .json extension.",
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

    MessageBoxW(dialog_,
                L"Profiles must be saved directly in the LayerC profiles "
                L"folder so LayerD can load them.",
                L"Choose the profiles folder", MB_OK | MB_ICONWARNING);
    initial_path = directory / candidate.filename();
  }
}

bool EditorWindow::save_profile(const bool force_save_as) {
  if (!validate_profile())
    return false;
  if (!visual_profile_ &&
      visual_sidecar_state_ == layerc::platform::VisualSidecarState::invalid) {
    MessageBoxW(dialog_,
                L"Repair the invalid visual sidecar with Calibrate before "
                L"saving this profile. This prevents an unrelated base edit "
                L"from silently discarding recovery data.",
                L"Visual sidecar needs repair", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!automation_profile_ &&
      automation_sidecar_state_ ==
          layerc::platform::AutomationSidecarState::invalid) {
    MessageBoxW(
        dialog_,
        L"Repair the invalid automation sidecar before saving this profile. "
        L"This prevents an unrelated base edit from discarding recovery data.",
        L"Automation sidecar needs repair", MB_OK | MB_ICONWARNING);
    return false;
  }
  auto destination = current_path_;
  if (force_save_as) {
    if (!choose_save_path(destination))
      return false;
  } else if (destination.empty()) {
    destination = layerc::platform::profile_directory() /
                  (to_wide(safe_identifier(to_wide(profile_.id))) + L".json");
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        MessageBoxW(
            dialog_, L"A profile with this ID already exists. Overwrite it?",
            L"Profile already exists", MB_YESNO | MB_ICONWARNING) != IDYES)
      return false;
    if (error) {
      MessageBoxW(dialog_, L"Could not inspect the profile destination.",
                  L"LayerC Editor", MB_OK | MB_ICONERROR);
      return false;
    }
  }
  if (visual_profile_)
    visual_profile_->profile_id = profile_.id;
  if (automation_profile_)
    automation_profile_->profile_id = profile_.id;
  if (!confirm_v2_upgrade())
    return false;
  if (!layerc::platform::write_profile_bundle(
          destination, profile_, visual_profile_, automation_profile_)) {
    MessageBoxW(dialog_,
                L"Could not save a consistent profile and visual-sidecar "
                L"bundle. The previous valid bundle was preserved.",
                L"Profile save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  current_path_ = destination;
  dirty_ = false;
  legacy_base_schema_ = false;
  legacy_visual_schema_ = false;
  v2_upgrade_acknowledged_ = false;
  history_.rebase(profile_, visual_profile_, automation_profile_);
  KillTimer(dialog_, autosave_timer);
  reload_profile_list(current_path_, false);
  notify_profile_committed(current_path_, profile_);
  set_status(L"Profile saved — LayerD updated live.");
  return true;
}

void EditorWindow::delete_profile() {
  if (current_path_.empty()) {
    MessageBoxW(dialog_, L"Save this profile before deleting it.",
                L"LayerC Editor",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!confirm_unsaved_changes())
    return;
  const auto prompt =
      L"Move '" + to_wide(profile_.name) + L"' to the Recycle Bin?";
  if (MessageBoxW(dialog_, prompt.c_str(), L"Delete profile",
                  MB_YESNO | MB_ICONWARNING) != IDYES)
    return;
  std::filesystem::path safe_remaining;
  for (const auto &entry : entries_) {
    if (entry.path.lexically_normal() != current_path_.lexically_normal()) {
      safe_remaining = entry.path;
      break;
    }
  }
  if (!recycle_profile_bundle(current_path_)) {
    MessageBoxW(dialog_, L"Could not move the profile to the Recycle Bin.",
                L"LayerC Editor", MB_OK | MB_ICONERROR);
    return;
  }
  end_edit_on_window();
  current_path_.clear();
  dirty_ = false;
  visual_profile_.reset();
  automation_profile_.reset();
  automation_sidecar_state_ = layerc::platform::AutomationSidecarState::absent;
  automation_sidecar_error_.clear();
  visual_sidecar_state_ = layerc::platform::VisualSidecarState::absent;
  visual_sidecar_error_.clear();
  (void)layerc::platform::signal_profile_reload();
  (void)layerc::platform::garbage_collect_unreferenced_visual_assets();
  reload_profile_list(safe_remaining, true);
  set_status(safe_remaining.empty() ? L"Profile moved to the Recycle Bin."
                                    : L"Profile deleted and LayerD reloaded.");
}

void EditorWindow::open_profiles_folder() {
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || directory.empty()) {
    MessageBoxW(dialog_, L"Could not open the profiles folder.",
                L"LayerC Editor",
                MB_OK | MB_ICONERROR);
    return;
  }
  ShellExecuteW(dialog_, L"open", directory.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

} // namespace layerc::editor
