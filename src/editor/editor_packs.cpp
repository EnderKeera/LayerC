#include "dialog_layout.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

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
#include <vector>

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
  DialogChrome chrome;
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
  DialogChrome chrome;
  bool updating{true};
};

void set_column(const HWND list, const int index, const int width,
                const wchar_t *text) {
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH;
  column.cx = width;
  column.pszText = const_cast<LPWSTR>(text);
  ListView_InsertColumn(list, index, &column);
}

void fit_pack_columns(const HWND list, const bool import, const int dpi) {
  if (list == nullptr)
    return;
  const HWND header = ListView_GetHeader(list);
  RECT client{};
  GetClientRect(header == nullptr ? list : header, &client);
  const auto scaled = [dpi](const int value) { return dpi_scaled(value, dpi); };
  const int available =
      std::max(1, static_cast<int>(client.right - client.left) - scaled(2));
  if (!import) {
    ListView_SetColumnWidth(list, 0, available);
    return;
  }
  const int action_width =
      std::min(scaled(120), std::max(scaled(88), available / 5));
  const int remaining = std::max(2, available - action_width);
  const int profile_width = std::max(1, remaining * 45 / 100);
  ListView_SetColumnWidth(list, 0, profile_width);
  ListView_SetColumnWidth(list, 1,
                          std::max(1, remaining - profile_width));
  ListView_SetColumnWidth(list, 2, action_width);
}

void layout_pack_dialog(const HWND window, const bool import) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const int dpi = window_base_dpi(window);
  const auto scaled = [dpi](const int value) { return dpi_scaled(value, dpi); };

  const int margin = scaled(12);
  const int gap = scaled(8);
  const int intro_height = scaled(30);
  const int button_height = scaled(25);
  const int action_top = height - margin - button_height;
  const int selection_top = action_top - gap - button_height;
  const int list_top = margin + intro_height + gap;
  move_control(window, IDC_PACK_INTRO, margin, margin, width - margin * 2,
                   intro_height);
  move_control(window, IDC_PACK_LIST, margin, list_top, width - margin * 2,
                   selection_top - gap - list_top);

  int left = margin;
  move_control(window, IDC_PACK_SELECT_ALL, left, selection_top, scaled(84),
                   button_height);
  left += scaled(92);
  if (import) {
    move_control(window, IDC_PACK_TOGGLE_ACTION, left, selection_top,
                     scaled(142), button_height);
    left += scaled(150);
  }
  move_control(window, IDC_PACK_SUMMARY, left,
                   selection_top + scaled(5), width - margin - left,
                   scaled(18));

  const int cancel_width = scaled(80);
  const int confirm_width = scaled(88);
  move_control(window, IDCANCEL, width - margin - cancel_width, action_top,
                   cancel_width, button_height);
  move_control(window, IDC_PACK_CONFIRM,
                   width - margin - cancel_width - gap - confirm_width,
                   action_top, confirm_width, button_height);
  fit_pack_columns(GetDlgItem(window, IDC_PACK_LIST), import, dpi);
}

bool handle_pack_window_message(const HWND window, const UINT message,
                                const WPARAM wparam, const LPARAM lparam,
                                const bool import, DialogChrome &chrome) {
  if (message == WM_SIZE) {
    layout_pack_dialog(window, import);
    return true;
  }
  if (message == WM_DPICHANGED) {
    (void)chrome.on_dpi_changed(HIWORD(wparam), lparam);
    layout_pack_dialog(window, import);
    return true;
  }
  if (message == WM_GETMINMAXINFO) {
    chrome.on_minimum_size(lparam, import ? 620 : 520, import ? 360 : 330);
    return true;
  }
  if (message == WM_DESTROY) {
    chrome.on_destroy();
    return true;
  }
  return false;
}

std::size_t checked_count(const HWND list) {
  std::size_t count{};
  for (int index = 0; index < ListView_GetItemCount(list); ++index)
    if (ListView_GetCheckState(list, index) != FALSE)
      ++count;
  return count;
}

void update_export_summary(const HWND window, ExportDialogState &state) {
  const HWND list = GetDlgItem(window, IDC_PACK_LIST);
  std::set<std::string, std::less<>> assets;
  for (int index = 0; index < ListView_GetItemCount(list); ++index) {
    if (ListView_GetCheckState(list, index) == FALSE)
      continue;
    assets.insert(state.items[static_cast<std::size_t>(index)].assets.begin(),
                  state.items[static_cast<std::size_t>(index)].assets.end());
  }
  std::uintmax_t bytes{};
  for (const auto &hash : assets) {
    const auto found = state.asset_sizes.find(hash);
    if (found != state.asset_sizes.end())
      bytes += found->second;
  }
  const auto count = checked_count(list);
  const auto text = std::to_wstring(count) + L" profile" +
                    (count == 1 ? L"" : L"s") + L" selected — " +
                    std::to_wstring((bytes + 1023U) / 1024U) +
                    L" KiB deduplicated assets";
  SetWindowTextW(GetDlgItem(window, IDC_PACK_SUMMARY), text.c_str());
  EnableWindow(GetDlgItem(window, IDC_PACK_CONFIRM), count != 0);
}

void update_import_summary(const HWND window, ImportDialogState &state) {
  if (state.updating) return;
  state.updating = true;
  const HWND list = GetDlgItem(window, IDC_PACK_LIST);
  state.selected.clear();
  for (std::size_t index = 0; index < state.items.size(); ++index) {
    if (ListView_GetCheckState(list, static_cast<int>(index)))
      state.selected.push_back({.profile_index = index,
                                .action = state.items[index].action});
  }
  std::wstring text = std::to_wstring(state.selected.size()) + L" profiles selected";
  bool valid = !state.selected.empty();
  if (valid) {
    const auto preview = layerc::bundle::preview_profile_import(
        *state.pack, state.selected, layerc::platform::profile_directory());
    valid = !preview.error.has_value();
    if (preview.error) {
      text = layerc::platform::utf8_to_wide(preview.error->message);
    } else {
      for (std::size_t index = 0; index < preview.entries.size(); ++index) {
        const auto &entry = preview.entries[index];
        auto &choice = state.selected[index];
        choice.expected_target = entry.target;
        choice.require_absent = choice.action == layerc::bundle::ImportAction::copy ||
                                !entry.collides;
        auto &item = state.items[entry.profile_index];
        item.filename = entry.target.filename().wstring();
        ListView_SetItemText(list, static_cast<int>(entry.profile_index), 1,
                            item.filename.data());
      }
    }
  }
  SetWindowTextW(GetDlgItem(window, IDC_PACK_SUMMARY), text.c_str());
  EnableWindow(GetDlgItem(window, IDC_PACK_CONFIRM), valid);
  state.updating = false;
}

void set_import_action_text(const HWND list, const int index,
                            const ImportItem &item) {
  const wchar_t *text = L"Import";
  if (item.conflict)
    text = item.action == layerc::bundle::ImportAction::copy ? L"Import Copy"
                                                             : L"Replace";
  ListView_SetItemText(list, index, 2, const_cast<LPWSTR>(text));
}

std::optional<std::filesystem::path> choose_pack_path(const HWND owner,
                                                      const bool save) {
  std::vector<wchar_t> buffer(32'768);
  const auto initial_directory = layerc::platform::profile_directory().wstring();
  const std::wstring initial_text = save ? L"LayerC profiles.layercprofile" : L"";
  if (initial_text.size() < buffer.size())
    std::copy(initial_text.begin(), initial_text.end(), buffer.begin());
  constexpr wchar_t filter[] =
      L"LayerC profile packs (*.layercprofile)\0*.layercprofile\0"
      L"All files (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = buffer.data();
  dialog.lpstrInitialDir = initial_directory.c_str();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.Flags =
      static_cast<DWORD>(OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                         (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST));
  dialog.lpstrDefExt = L"layercprofile";
  const BOOL chosen =
      save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) {
    const auto error = CommDlgExtendedError();
    if (error != 0) {
      const auto message = L"Could not open the profile pack chooser (error " +
                           std::to_wstring(error) + L").";
      MessageBoxW(owner, message.c_str(), L"Profile pack", MB_OK | MB_ICONERROR);
    }
    return std::nullopt;
  }
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

INT_PTR CALLBACK EditorWindow::export_pack_dialog_proc(const HWND window,
                                                       const UINT message,
                                                       const WPARAM wparam,
                                                       const LPARAM lparam) {
  auto *state = bind_dialog_state<ExportDialogState>(window, message, lparam);
  if (state == nullptr)
    return FALSE;
  if (message == WM_INITDIALOG) {
    apply_dialog_chrome(window, state->editor->shell_.capture_exclusion());
    if (!state->chrome.attach(window))
      state->editor->style_dialog_children(window);
    const HWND list = GetDlgItem(window, IDC_PACK_LIST);
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                  LVS_EX_LABELTIP);
    set_column(list, 0, 1, L"Saved profile");
    state->editor->style_list_view(list, false);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
      LVITEMW item{};
      item.mask = LVIF_TEXT | LVIF_PARAM;
      item.iItem = static_cast<int>(index);
      item.pszText = state->items[index].label.data();
      item.lParam = static_cast<LPARAM>(index);
      ListView_InsertItem(list, &item);
      ListView_SetCheckState(list, static_cast<int>(index),
                             index == state->current_index);
    }
    update_export_summary(window, *state);
    layout_pack_dialog(window, false);
    return TRUE;
  }
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
      if (!state->selected.empty())
        EndDialog(window, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  if (handle_pack_window_message(window, message, wparam, lparam, false,
                                 state->chrome))
    return TRUE;
  return state->editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

INT_PTR CALLBACK EditorWindow::import_pack_dialog_proc(const HWND window,
                                                       const UINT message,
                                                       const WPARAM wparam,
                                                       const LPARAM lparam) {
  auto *state = bind_dialog_state<ImportDialogState>(window, message, lparam);
  if (state == nullptr)
    return FALSE;
  if (message == WM_INITDIALOG) {
    apply_dialog_chrome(window, state->editor->shell_.capture_exclusion());
    if (!state->chrome.attach(window))
      state->editor->style_dialog_children(window);
    const HWND list = GetDlgItem(window, IDC_PACK_LIST);
    ListView_SetExtendedListViewStyle(
        list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                  LVS_EX_LABELTIP);
    set_column(list, 0, 1, L"Profile");
    set_column(list, 1, 1, L"Destination file");
    set_column(list, 2, 1, L"Action");
    state->editor->style_list_view(list, false);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
      LVITEMW item{};
      item.mask = LVIF_TEXT | LVIF_PARAM;
      item.iItem = static_cast<int>(index);
      item.pszText = state->items[index].name.data();
      item.lParam = static_cast<LPARAM>(index);
      ListView_InsertItem(list, &item);
      ListView_SetItemText(list, static_cast<int>(index), 1,
                           state->items[index].filename.data());
      set_import_action_text(list, static_cast<int>(index),
                             state->items[index]);
      ListView_SetCheckState(list, static_cast<int>(index), TRUE);
    }
    state->updating = false;
    update_import_summary(window, *state);
    layout_pack_dialog(window, true);
    return TRUE;
  }
  if (message == WM_NOTIFY &&
      reinterpret_cast<NMHDR *>(lparam)->idFrom == IDC_PACK_LIST &&
      reinterpret_cast<NMHDR *>(lparam)->code == LVN_ITEMCHANGED) {
    const auto *change = reinterpret_cast<NMLISTVIEW *>(lparam);
    if (((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) != 0)
      update_import_summary(window, *state);
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_PACK_SELECT_ALL: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      for (int index = 0; index < ListView_GetItemCount(list); ++index)
        ListView_SetCheckState(list, index, TRUE);
      update_import_summary(window, *state);
      return TRUE;
    }
    case IDC_PACK_TOGGLE_ACTION: {
      const HWND list = GetDlgItem(window, IDC_PACK_LIST);
      int index = ListView_GetNextItem(list, -1, LVNI_SELECTED);
      if (index < 0)
        index = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
      if (index >= 0) {
        auto &item = state->items[static_cast<std::size_t>(index)];
        if (item.conflict) {
          item.action = item.action == layerc::bundle::ImportAction::copy
                            ? layerc::bundle::ImportAction::replace
                            : layerc::bundle::ImportAction::copy;
          set_import_action_text(list, index, item);
          update_import_summary(window, *state);
        }
      }
      return TRUE;
    }
    case IDC_PACK_CONFIRM: {
      if (!state->selected.empty() &&
          IsWindowEnabled(GetDlgItem(window, IDC_PACK_CONFIRM)))
        EndDialog(window, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  if (handle_pack_window_message(window, message, wparam, lparam, true,
                                 state->chrome))
    return TRUE;
  return state->editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

void EditorWindow::export_profile_pack() {
  if (document_.dirty() && !save_profile(false))
    return;
  ExportDialogState state{};
  state.editor = this;
  state.items.reserve(shell_.profiles().size());
  for (std::size_t index = 0; index < shell_.profiles().size(); ++index) {
    ExportItem item{};
    item.path = shell_.profiles()[index].path;
    item.label = shell_.profiles()[index].label;
    if (shell_.profiles()[index].path.lexically_normal() ==
        document_.path().lexically_normal())
      state.current_index = index;
    const auto base = layerc::platform::read_profile_file(shell_.profiles()[index].path);
    if (base.has_value()) {
      const auto parsed = layerc::core::parse_profile_json(*base);
      if (const auto *profile =
              std::get_if<layerc::core::GameProfile>(&parsed)) {
        const auto visual = layerc::platform::load_visual_sidecar(
            shell_.profiles()[index].path, *profile);
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
            if (!size_error)
              state.asset_sizes.try_emplace(hash, size);
          }
        }
      }
    }
    state.items.push_back(std::move(item));
  }
  if (state.items.empty()) {
    MessageBoxW(shell_.window(), L"There are no saved profiles to export.",
                L"Export Pack", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (state.current_index == SIZE_MAX)
    state.current_index = 0;
  if (DialogBoxParamW(shell_.instance(), MAKEINTRESOURCEW(IDD_EXPORT_PACK), shell_.window(),
                      &EditorWindow::export_pack_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK)
    return;
  const auto destination = choose_pack_path(shell_.window(), true);
  if (!destination.has_value())
    return;
  const auto result =
      layerc::bundle::export_profile_pack(*destination, state.selected);
  if (const auto *error = std::get_if<layerc::bundle::PackError>(&result)) {
    show_pack_error(shell_.window(), L"Profile pack export failed", *error);
    return;
  }
  set_status(L"Profile pack exported.");
}

void EditorWindow::import_profile_pack() {
  if (!confirm_unsaved_changes())
    return;
  const auto source = choose_pack_path(shell_.window(), false);
  if (!source.has_value())
    return;
  const auto validation = layerc::bundle::validate_profile_pack(*source);
  const auto *pack = std::get_if<layerc::bundle::ValidatedPack>(&validation);
  if (pack == nullptr) {
    show_pack_error(shell_.window(), L"Profile pack validation failed",
                    std::get<layerc::bundle::PackError>(validation));
    return;
  }

  const auto profile_directory = layerc::platform::profile_directory();
  std::vector<layerc::bundle::ImportChoice> copy_choices;
  for (std::size_t index = 0; index < pack->profiles.size(); ++index)
    copy_choices.push_back({.profile_index = index,
                            .action = layerc::bundle::ImportAction::copy});
  const auto preview = layerc::bundle::preview_profile_import(
      *pack, copy_choices, profile_directory);
  if (preview.error) {
    show_pack_error(shell_.window(), L"Import preview failed", *preview.error);
    return;
  }
  ImportDialogState state{};
  state.editor = this;
  state.pack = pack;
  state.items.reserve(pack->profiles.size());
  for (const auto &entry : preview.entries) {
    const auto &profile = pack->profiles[entry.profile_index];
    state.items.push_back(
        {.name = layerc::platform::utf8_to_wide(profile.profile.name),
         .filename = entry.target.filename().wstring(),
         .conflict = entry.collides,
         .action = entry.collides ? layerc::bundle::ImportAction::copy
                                  : layerc::bundle::ImportAction::replace});
  }
  if (DialogBoxParamW(shell_.instance(), MAKEINTRESOURCEW(IDD_IMPORT_PACK), shell_.window(),
                      &EditorWindow::import_pack_dialog_proc,
                      reinterpret_cast<LPARAM>(&state)) != IDOK)
    return;
  const auto result = layerc::bundle::import_profile_pack(
      *pack, state.selected, layerc::platform::profile_directory());
  if (const auto *error = std::get_if<layerc::bundle::PackError>(&result)) {
    show_pack_error(shell_.window(), L"Profile pack import failed", *error);
    return;
  }
  reload_profile_list({}, true);
  set_status(L"Profile pack imported — LayerD reloaded.");
}

} // namespace layerc::editor
