#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <variant>

#include <commctrl.h>
#include <commdlg.h>

namespace layerc::editor {
namespace {

struct ExportItem {
  std::filesystem::path path;
  std::wstring label;
  std::vector<std::string> assets;
};

struct ExportDialogState {
  EditorWindow *editor{};
  std::vector<ExportItem> items;
  std::map<std::string, std::uintmax_t, std::less<>> asset_sizes;
  std::size_t current_index{SIZE_MAX};
  std::vector<std::filesystem::path> selected;
};

struct ImportItem {
  std::wstring name;
  std::wstring filename;
  bool conflict{};
  layerc::bundle::ImportAction action{layerc::bundle::ImportAction::replace};
};

struct ImportDialogState {
  EditorWindow *editor{};
  const layerc::bundle::ValidatedPack *pack{};
  std::vector<ImportItem> items;
  std::vector<layerc::bundle::ImportChoice> selected;
};

void set_column(const HWND list, const int index, const int width,
                const wchar_t *text) {
  LVCOLUMNW column{.mask = LVCF_TEXT | LVCF_WIDTH,
                   .cx = width,
                   .pszText = const_cast<LPWSTR>(text)};
  ListView_InsertColumn(list, index, &column);
}

std::size_t checked_count(const HWND list) {
  std::size_t count{};
  for (int index = 0; index < ListView_GetItemCount(list); ++index)
    if (ListView_GetCheckState(list, index) != FALSE) ++count;
  return count;
}

void update_export_summary(const HWND window, ExportDialogState &state) {
  const HWND list = GetDlgItem(window, IDC_PACK_LIST);
  std::set<std::string, std::less<>> assets;
  for (int index = 0; index < ListView_GetItemCount(list); ++index) {
    if (ListView_GetCheckState(list, index) == FALSE) continue;
    assets.insert(state.items[static_cast<std::size_t>(index)].assets.begin(),
                  state.items[static_cast<std::size_t>(index)].assets.end());
  }
  std::uintmax_t bytes{};
  for (const auto &hash : assets) {
    const auto found = state.asset_sizes.find(hash);
    if (found != state.asset_sizes.end()) bytes += found->second;
  }
  const auto count = checked_count(list);
  const auto text = std::to_wstring(count) + L" profile" +
                    (count == 1 ? L"" : L"s") + L" selected — " +
                    std::to_wstring((bytes + 1023U) / 1024U) +
                    L" KiB deduplicated assets";
  SetWindowTextW(GetDlgItem(window, IDC_PACK_SUMMARY), text.c_str());
  EnableWindow(GetDlgItem(window, IDC_PACK_CONFIRM), count != 0);
}

void update_import_summary(const HWND window) {
  const auto count = checked_count(GetDlgItem(window, IDC_PACK_LIST));
  const auto text = std::to_wstring(count) + L" profile" +
                    (count == 1 ? L"" : L"s") + L" selected";
  SetWindowTextW(GetDlgItem(window, IDC_PACK_SUMMARY), text.c_str());
  EnableWindow(GetDlgItem(window, IDC_PACK_CONFIRM), count != 0);
}

void set_import_action_text(const HWND list, const int index,
                            const ImportItem &item) {
  const wchar_t *text = L"Import";
  if (item.conflict)
    text = item.action == layerc::bundle::ImportAction::copy
               ? L"Import Copy"
               : L"Replace";
  ListView_SetItemText(list, index, 2, const_cast<LPWSTR>(text));
}

std::optional<std::filesystem::path> choose_pack_path(
    const HWND owner, const bool save) {
  std::array<wchar_t, 32'768> buffer{};
  const auto initial =
      layerc::platform::profile_directory() /
      (save ? L"LayerC profiles.layercprofile" : L"");
  const auto initial_text = initial.wstring();
  if (initial_text.size() < buffer.size())
    std::copy(initial_text.begin(), initial_text.end(), buffer.begin());
  constexpr wchar_t filter[] =
      L"LayerC profile packs (*.layercprofile)\0*.layercprofile\0"
      L"All files (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{.lStructSize = sizeof(dialog),
                       .hwndOwner = owner,
                       .lpstrFilter = filter,
                       .lpstrFile = buffer.data(),
                       .nMaxFile = static_cast<DWORD>(buffer.size()),
                       .Flags = static_cast<DWORD>(
                           OFN_EXPLORER | OFN_PATHMUSTEXIST |
                           (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST)),
                       .lpstrDefExt = L"layercprofile"};
  const BOOL chosen =
      save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) return std::nullopt;
  std::filesystem::path path(buffer.data());
  if (_wcsicmp(path.extension().c_str(), L".layercprofile") != 0)
    path.replace_extension(L".layercprofile");
  return path;
}

void show_pack_error(const HWND owner, const wchar_t *title,
                     const layerc::bundle::PackError &error) {
  const auto message = layerc::platform::utf8_to_wide(error.message);
  MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONERROR);
}

} // namespace

INT_PTR CALLBACK EditorWindow::export_pack_dialog_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  auto *state = reinterpret_cast<ExportDialogState *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<ExportDialogState *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    set_dark_title_bar(window);
    (void)layerc::platform::apply_capture_exclusion(
        window, state->editor->capture_exclusion_);
    state->editor->style_dialog_children(window);
    const HWND list = GetDlgItem(window, IDC_PACK_LIST);
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                  LVS_EX_LABELTIP);
    set_column(list, 0, 430, L"Saved profile");
    state->editor->style_list_view(list, false);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
      LVITEMW item{.mask = LVIF_TEXT | LVIF_PARAM,
                   .iItem = static_cast<int>(index),
                   .pszText = state->items[index].label.data(),
                   .lParam = static_cast<LPARAM>(index)};
      ListView_InsertItem(list, &item);
      ListView_SetCheckState(list, static_cast<int>(index),
                             index == state->current_index);
    }
    update_export_summary(window, *state);
    return TRUE;
  }
  if (state == nullptr) return FALSE;
  if (message == WM_NOTIFY &&
      reinterpret_cast<NMHDR *>(lparam)->idFrom == IDC_PACK_LIST &&
      reinterpret_cast<NMHDR *>(lparam)->code == LVN_ITEMCHANGED) {
    update_export_summary(window, *state);
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_PACK_SELECT_ALL: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      for (int index = 0; index < ListView_GetItemCount(list); ++index)
        ListView_SetCheckState(list, index, TRUE);
      update_export_summary(window, *state);
      return TRUE;
    }
    case IDC_PACK_CONFIRM: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      state->selected.clear();
      for (int index = 0; index < ListView_GetItemCount(list); ++index)
        if (ListView_GetCheckState(list, index) != FALSE)
          state->selected.push_back(
              state->items[static_cast<std::size_t>(index)].path);
      if (!state->selected.empty()) EndDialog(window, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_DRAWITEM) {
    state->editor->draw_button(
        *reinterpret_cast<DRAWITEMSTRUCT *>(lparam),
        layerc::platform::EditorControlRole::button);
    return TRUE;
  }
  if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
    return state->editor->color_message(message, wparam, lparam);
  return FALSE;
}

INT_PTR CALLBACK EditorWindow::import_pack_dialog_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  auto *state = reinterpret_cast<ImportDialogState *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<ImportDialogState *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    set_dark_title_bar(window);
    (void)layerc::platform::apply_capture_exclusion(
        window, state->editor->capture_exclusion_);
    state->editor->style_dialog_children(window);
    const HWND list = GetDlgItem(window, IDC_PACK_LIST);
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                  LVS_EX_LABELTIP);
    set_column(list, 0, 245, L"Profile");
    set_column(list, 1, 190, L"Suggested file");
    set_column(list, 2, 125, L"Action");
    state->editor->style_list_view(list, false);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
      LVITEMW item{.mask = LVIF_TEXT | LVIF_PARAM,
                   .iItem = static_cast<int>(index),
                   .pszText = state->items[index].name.data(),
                   .lParam = static_cast<LPARAM>(index)};
      ListView_InsertItem(list, &item);
      ListView_SetItemText(list, static_cast<int>(index), 1,
                           state->items[index].filename.data());
      set_import_action_text(list, static_cast<int>(index),
                             state->items[index]);
      ListView_SetCheckState(list, static_cast<int>(index), TRUE);
    }
    update_import_summary(window);
    return TRUE;
  }
  if (state == nullptr) return FALSE;
  if (message == WM_NOTIFY &&
      reinterpret_cast<NMHDR *>(lparam)->idFrom == IDC_PACK_LIST &&
      reinterpret_cast<NMHDR *>(lparam)->code == LVN_ITEMCHANGED) {
    update_import_summary(window);
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_PACK_SELECT_ALL: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      for (int index = 0; index < ListView_GetItemCount(list); ++index)
        ListView_SetCheckState(list, index, TRUE);
      update_import_summary(window);
      return TRUE;
    }
    case IDC_PACK_TOGGLE_ACTION: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
      if (index < 0) index = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
      if (index >= 0) {
        auto &item = state->items[static_cast<std::size_t>(index)];
        if (item.conflict) {
          item.action =
              item.action == layerc::bundle::ImportAction::copy
                  ? layerc::bundle::ImportAction::replace
                  : layerc::bundle::ImportAction::copy;
          set_import_action_text(list, index, item);
        }
      }
      return TRUE;
    }
    case IDC_PACK_CONFIRM: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      state->selected.clear();
      for (int index = 0; index < ListView_GetItemCount(list); ++index) {
        if (ListView_GetCheckState(list, index) == FALSE) continue;
        state->selected.push_back(
            {.profile_index = static_cast<std::size_t>(index),
             .action =
                 state->items[static_cast<std::size_t>(index)].action});
      }
      if (!state->selected.empty()) EndDialog(window, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  if (message == WM_DRAWITEM) {
    state->editor->draw_button(
        *reinterpret_cast<DRAWITEMSTRUCT *>(lparam),
        layerc::platform::EditorControlRole::button);
    return TRUE;
  }
  if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
    return state->editor->color_message(message, wparam, lparam);
  return FALSE;
}

void EditorWindow::export_profile_pack() {
  if (dirty_ && !save_profile(false)) return;
  ExportDialogState state{.editor = this};
  state.items.reserve(entries_.size());
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    ExportItem item{.path = entries_[index].path,
                    .label = entries_[index].label};
    if (entries_[index].path.lexically_normal() ==
        current_path_.lexically_normal())
      state.current_index = index;
    const auto base =
        layerc::platform::read_profile_file(entries_[index].path);
    if (base.has_value()) {
      const auto parsed = layerc::core::parse_profile_json(*base);
      if (const auto *profile =
              std::get_if<layerc::core::GameProfile>(&parsed)) {
        const auto visual = layerc::platform::load_visual_sidecar(
            entries_[index].path, *profile);
        if (visual.state == layerc::platform::VisualSidecarState::valid &&
            visual.profile.has_value()) {
          for (const auto &definition : visual.profile->templates)
            for (const auto &reference : definition.references)
              item.assets.push_back(reference.asset_sha256);
          std::ranges::sort(item.assets);
          item.assets.erase(std::unique(item.assets.begin(), item.assets.end()),
                            item.assets.end());
          for (const auto &hash : item.assets) {
            std::error_code size_error;
            const auto size = std::filesystem::file_size(
                layerc::platform::visual_asset_path(hash), size_error);
            if (!size_error) state.asset_sizes.try_emplace(hash, size);
          }
        }
      }
    }
    state.items.push_back(std::move(item));
  }
  if (state.items.empty()) {
    MessageBoxW(dialog_, L"There are no saved profiles to export.", L"Export Pack",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (state.current_index == SIZE_MAX) state.current_index = 0;
  if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_EXPORT_PACK), dialog_,
                      &EditorWindow::export_pack_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK)
    return;
  const auto destination = choose_pack_path(dialog_, true);
  if (!destination.has_value()) return;
  const auto result =
      layerc::bundle::export_profile_pack(*destination, state.selected);
  if (const auto *error = std::get_if<layerc::bundle::PackError>(&result)) {
    show_pack_error(dialog_, L"Profile pack export failed", *error);
    return;
  }
  set_status(L"Profile pack exported.");
}

void EditorWindow::import_profile_pack() {
  if (!confirm_unsaved_changes()) return;
  const auto source = choose_pack_path(dialog_, false);
  if (!source.has_value()) return;
  const auto validation = layerc::bundle::validate_profile_pack(*source);
  const auto *pack = std::get_if<layerc::bundle::ValidatedPack>(&validation);
  if (pack == nullptr) {
    show_pack_error(dialog_, L"Profile pack validation failed",
                    std::get<layerc::bundle::PackError>(validation));
    return;
  }

  std::set<std::string, std::less<>> existing_ids;
  std::set<std::wstring, std::less<>> existing_files;
  std::error_code directory_error;
  const auto profile_directory = layerc::platform::profile_directory();
  if (std::filesystem::exists(profile_directory, directory_error)) {
    for (std::filesystem::directory_iterator iterator(profile_directory,
                                                         directory_error),
         end;
         !directory_error && iterator != end;
         iterator.increment(directory_error)) {
      if (!iterator->is_regular_file(directory_error) || directory_error)
        continue;
      if (_wcsicmp(iterator->path().extension().c_str(), L".json") == 0)
        existing_files.insert(layerc::platform::lower_copy(
            iterator->path().filename().wstring()));
    }
  }
  if (directory_error) {
    MessageBoxW(dialog_, L"The profiles folder could not be enumerated.",
                L"Import Pack", MB_OK | MB_ICONERROR);
    return;
  }
  for (const auto &entry : entries_) {
    const auto json = layerc::platform::read_profile_file(entry.path);
    if (!json.has_value()) continue;
    const auto parsed = layerc::core::parse_profile_json(*json);
    if (const auto *profile =
            std::get_if<layerc::core::GameProfile>(&parsed))
      existing_ids.insert(profile->id);
  }
  ImportDialogState state{.editor = this, .pack = pack};
  state.items.reserve(pack->profiles.size());
  for (const auto &profile : pack->profiles) {
    const auto suggested = layerc::bundle::resolve_suggested_profile_filename(
        profile.suggested_filename);
    if (!suggested.has_value()) {
      MessageBoxW(dialog_, L"The pack contains an invalid suggested filename.",
                  L"Import Pack", MB_OK | MB_ICONERROR);
      return;
    }
    const auto filename = suggested->wstring();
    const bool conflict =
        existing_ids.contains(profile.profile.id) ||
        existing_files.contains(layerc::platform::lower_copy(filename));
    state.items.push_back(
        {.name = layerc::platform::utf8_to_wide(profile.profile.name),
         .filename = filename,
         .conflict = conflict,
         .action = conflict ? layerc::bundle::ImportAction::copy
                            : layerc::bundle::ImportAction::replace});
  }
  if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_IMPORT_PACK), dialog_,
                      &EditorWindow::import_pack_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK)
    return;
  const auto result = layerc::bundle::import_profile_pack(
      *pack, state.selected, layerc::platform::profile_directory());
  if (const auto *error = std::get_if<layerc::bundle::PackError>(&result)) {
    show_pack_error(dialog_, L"Profile pack import failed", *error);
    return;
  }
  reload_profile_list({}, true);
  set_status(L"Profile pack imported — LayerD reloaded.");
}

} // namespace layerc::editor
