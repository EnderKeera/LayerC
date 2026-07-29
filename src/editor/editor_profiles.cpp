#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/platform/profile_store.hpp"

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
  if (syncing_) return;
  profile_.name = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_NAME)));
  profile_.id = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_ID)));
  if (bar_controls_panel_ < profile_.panels.size()) {
    auto &panel = profile_.panels[bar_controls_panel_];
    panel.bar.width = parse_uint(control_text(GetDlgItem(dialog_, IDC_BAR_WIDTH)), panel.bar.width);
    panel.bar.height = parse_uint(control_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT)), panel.bar.height);
    panel.bar.show_text =
        owner_draw_checkbox_checked(GetDlgItem(dialog_, IDC_SHOW_TEXT));
    panel.bar.hide_when_ready =
        owner_draw_checkbox_checked(GetDlgItem(dialog_, IDC_HIDE_READY));
  }
}

bool EditorWindow::validate_profile() {
  sync_profile_from_controls();
  const auto parsed = layerc::core::parse_profile_json(layerc::core::serialize_profile_json(profile_));
  if (const auto *error = std::get_if<layerc::core::ProfileError>(&parsed)) {
    MessageBoxW(dialog_, profile_error_text(*error).c_str(), L"Profile is incomplete", MB_OK | MB_ICONWARNING);
    return false;
  }
  profile_ = std::get<layerc::core::GameProfile>(parsed);
  return true;
}

bool EditorWindow::confirm_unsaved_changes() {
  if (!dirty_) return true;
  const int choice = MessageBoxW(dialog_, L"The current profile has unsaved changes. Save them first?",
                                 L"Unsaved changes", MB_YESNOCANCEL | MB_ICONWARNING);
  return choice == IDYES ? save_profile(false) : choice == IDNO;
}

void EditorWindow::new_profile(const bool confirm) {
  if (confirm && !confirm_unsaved_changes()) return;
  profile_ = make_new_profile();
  current_path_.clear();
  dirty_ = true;
  refresh_profile_controls();
  set_status(L"New profile ready — choose a task, configure bars, then Attach.");
}
void EditorWindow::reload_profile_list(const std::filesystem::path &select_path,
                                       const bool open_selection) {
  entries_.clear();
  syncing_ = true;
  ListView_DeleteAllItems(profile_list_);
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  if (!directory.empty() && std::filesystem::exists(directory, error) && !error) {
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".json") != 0) continue;
      const auto contents = layerc::platform::read_profile_file(entry.path());
      if (!contents.has_value()) continue;
      const auto parsed = layerc::core::parse_profile_json(*contents);
      const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
      if (profile == nullptr) continue;
      entries_.push_back({.path = entry.path(),
                          .label = profile_list_label(*profile),
                          .enabled = profile->enabled});
    }
  }
  std::sort(entries_.begin(), entries_.end(), [](const ProfileEntry &left, const ProfileEntry &right) {
    return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
  });
  int selected = -1;
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    LVITEMW item{.mask = LVIF_TEXT | LVIF_PARAM, .iItem = static_cast<int>(index),
                 .pszText = const_cast<LPWSTR>(entries_[index].label.c_str()),
                 .lParam = static_cast<LPARAM>(index)};
    ListView_InsertItem(profile_list_, &item);
    ListView_SetCheckState(profile_list_, static_cast<int>(index), entries_[index].enabled);
    if (!select_path.empty() && entries_[index].path.lexically_normal() == select_path.lexically_normal())
      selected = static_cast<int>(index);
  }
  if (selected < 0 && !entries_.empty()) selected = 0;
  if (selected >= 0) ListView_SetItemState(profile_list_, selected, LVIS_SELECTED | LVIS_FOCUSED,
                                             LVIS_SELECTED | LVIS_FOCUSED);
  syncing_ = false;
  if (open_selection && selected >= 0) open_profile_row(selected, false);
  else if (entries_.empty() && current_path_.empty()) new_profile(false);
}

void EditorWindow::open_profile_row(const int row, const bool confirm) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) return;
  const auto target_path = entries_[static_cast<std::size_t>(row)].path;
  const auto select_path = [&](const std::filesystem::path &path) {
    syncing_ = true;
    ListView_SetItemState(profile_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].path.lexically_normal() != path.lexically_normal()) continue;
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
  current_path_ = target_path;
  dirty_ = false;
  select_path(target_path);
  refresh_profile_controls();
  set_status(profile_.enabled ? L"Profile loaded." : L"Disabled profile loaded.");
}

void EditorWindow::save_profile_enabled(const int row, const bool enabled) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) return;
  auto &entry = entries_[static_cast<std::size_t>(row)];
  const auto restore_checkbox = [&] {
    const bool was_syncing = syncing_;
    syncing_ = true;
    ListView_SetCheckState(profile_list_, row, entry.enabled);
    syncing_ = was_syncing;
  };
  const auto contents = layerc::platform::read_profile_file(entry.path);
  if (!contents.has_value()) {
    restore_checkbox();
    set_status(L"Could not update the selected profile.");
    return;
  }
  auto parsed = layerc::core::parse_profile_json(*contents);
  auto *stored = std::get_if<layerc::core::GameProfile>(&parsed);
  if (stored == nullptr) {
    restore_checkbox();
    set_status(L"Could not update an invalid profile.");
    return;
  }
  stored->enabled = enabled;
  if (!layerc::platform::write_profile_file(entry.path, layerc::core::serialize_profile_json(*stored))) {
    restore_checkbox();
    set_status(L"Could not save the profile enablement change.");
    return;
  }
  entry.enabled = enabled;
  entry.label = profile_list_label(*stored);
  const bool was_syncing = syncing_;
  syncing_ = true;
  ListView_SetItemText(profile_list_, row, 0, entry.label.data());
  syncing_ = was_syncing;
  if (entry.path.lexically_normal() == current_path_.lexically_normal())
    profile_.enabled = enabled;
  (void)layerc::platform::signal_profile_reload();
  set_status(enabled ? L"Profile enabled — LayerD reloaded."
                     : L"Profile disabled — LayerD retired its instances.");
  InvalidateRect(profile_list_, nullptr, TRUE);
}

bool EditorWindow::choose_save_path(std::filesystem::path &path) {
  const auto directory = layerc::platform::profile_directory();
  if (directory.empty()) {
    MessageBoxW(dialog_, L"The profiles directory is unavailable.", L"LayerE",
                MB_OK | MB_ICONERROR);
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    MessageBoxW(dialog_, L"Could not create the profiles directory.", L"LayerE",
                MB_OK | MB_ICONERROR);
    return false;
  }

  const auto initial_filename = path.empty()
      ? std::filesystem::path(to_wide(safe_identifier(to_wide(profile_.id))) + L".json")
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
    if (GetSaveFileNameW(&dialog) == FALSE) return false;

    auto candidate = std::filesystem::path(buffer.data());
    if (candidate.extension().empty()) candidate += L".json";
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
    const auto canonical_parent =
        std::filesystem::weakly_canonical(candidate.parent_path(), parent_error);
    if (!directory_error && !parent_error &&
        _wcsicmp(canonical_directory.c_str(), canonical_parent.c_str()) == 0) {
      path = std::move(candidate);
      return true;
    }

    MessageBoxW(dialog_,
                L"Profiles must be saved directly in the LayerC profiles folder so LayerD can load them.",
                L"Choose the profiles folder", MB_OK | MB_ICONWARNING);
    initial_path = directory / candidate.filename();
  }
}

bool EditorWindow::save_profile(const bool force_save_as) {
  if (!validate_profile()) return false;
  auto destination = current_path_;
  if (force_save_as) {
    if (!choose_save_path(destination)) return false;
  } else if (destination.empty()) {
    destination = layerc::platform::profile_directory() / (to_wide(safe_identifier(to_wide(profile_.id))) + L".json");
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        MessageBoxW(dialog_, L"A profile with this ID already exists. Overwrite it?",
                    L"Profile already exists", MB_YESNO | MB_ICONWARNING) != IDYES) return false;
    if (error) { MessageBoxW(dialog_, L"Could not inspect the profile destination.", L"LayerE", MB_OK | MB_ICONERROR); return false; }
  }
  if (!layerc::platform::write_profile_file(destination, layerc::core::serialize_profile_json(profile_))) {
    MessageBoxW(dialog_, L"Could not save the profile file.", L"LayerE", MB_OK | MB_ICONERROR);
    return false;
  }
  current_path_ = destination;
  dirty_ = false;
  reload_profile_list(current_path_, false);
  (void)layerc::platform::signal_profile_reload();
  set_status(L"Profile saved — LayerD reloaded.");
  return true;
}

void EditorWindow::delete_profile() {
  if (current_path_.empty()) {
    MessageBoxW(dialog_, L"Save this profile before deleting it.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto prompt = L"Move '" + to_wide(profile_.name) + L"' to the Recycle Bin?";
  if (MessageBoxW(dialog_, prompt.c_str(), L"Delete profile", MB_YESNO | MB_ICONWARNING) != IDYES) return;
  std::filesystem::path safe_remaining;
  for (const auto &entry : entries_) {
    if (entry.path.lexically_normal() != current_path_.lexically_normal()) { safe_remaining = entry.path; break; }
  }
  if (!recycle_profile_file(current_path_)) {
    MessageBoxW(dialog_, L"Could not move the profile to the Recycle Bin.", L"LayerE", MB_OK | MB_ICONERROR);
    return;
  }
  current_path_.clear(); dirty_ = false;
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
    MessageBoxW(dialog_, L"Could not open the profiles folder.", L"LayerE", MB_OK | MB_ICONERROR);
    return;
  }
  ShellExecuteW(dialog_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void EditorWindow::show_file_menu() {
  const HMENU menu = LoadMenuW(instance_, MAKEINTRESOURCEW(IDR_EDITOR_MENU));
  const HMENU popup = menu == nullptr ? nullptr : GetSubMenu(menu, 0);
  if (popup == nullptr) {
    if (menu != nullptr) DestroyMenu(menu);
    set_status(L"Could not open the File menu.");
    return;
  }
  const bool has_grouped_bars = std::any_of(
      profile_.panels.begin(), profile_.panels.end(),
      [](const layerc::core::Panel &panel) {
        return panel.style == layerc::core::PanelStyle::bar &&
               panel.cooldown_ids.size() > 1;
      });
  EnableMenuItem(popup, IDM_FILE_SPLIT_BARS,
                 MF_BYCOMMAND |
                     (has_grouped_bars ? MF_ENABLED : MF_GRAYED));
  RECT button{};
  GetWindowRect(GetDlgItem(dialog_, IDC_FILE_MENU), &button);
  SetForegroundWindow(dialog_);
  TrackPopupMenuEx(popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON |
                              TPM_RIGHTBUTTON,
                   button.left, button.bottom, dialog_, nullptr);
  PostMessageW(dialog_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}
void EditorWindow::attach_profile() {
  if (!profile_.enabled) {
    MessageBoxW(dialog_,
                L"This profile is disabled. Enable it in Saved Profiles before attaching.",
                L"Profile disabled", MB_OK | MB_ICONINFORMATION);
    set_status(L"Profile is disabled — enable it before attaching.");
    return;
  }
  if (!save_profile(false)) return;
  if (layerc::platform::signal_profile_reload()) set_status(L"Saved — LayerD reloaded and will attach automatically.");
  else if (start_daemon()) set_status(L"Saved — LayerD started and will attach automatically.");
  else MessageBoxW(dialog_, L"Profile was saved, but LayerD could not be started.", L"LayerE", MB_OK | MB_ICONWARNING);
}

} // namespace layerc::editor
