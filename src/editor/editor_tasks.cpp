#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <utility>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {

namespace {

void show_task_actions_menu(const HWND window) {
  const HMENU menu = CreatePopupMenu();
  if (menu == nullptr)
    return;
  AppendMenuW(menu, MF_STRING, IDC_EDIT_SELECTED_TASK,
              L"&Position on Window");
  AppendMenuW(menu, MF_STRING, IDC_CALIBRATE_SELECTED_TASK,
              L"&Calibrate Visual");
  RECT anchor{};
  const HWND button = GetDlgItem(window, IDC_TASK_ACTIONS);
  if (button != nullptr && GetWindowRect(button, &anchor) != FALSE) {
    const UINT command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        anchor.left, anchor.bottom, 0, window, nullptr));
    if (command != 0U)
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
  }
  DestroyMenu(menu);
}

} // namespace

void EditorWindow::initialize_tasks(const HWND window) {
  task_dialog_ = window;
  const UINT window_dpi = GetDpiForWindow(window);
  const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
  SetWindowPos(window, nullptr, 0, 0, MulDiv(760, dpi, 96),
               MulDiv(360, dpi, 96),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  task_font_ = create_editor_font(static_cast<UINT>(dpi));
  set_dark_title_bar(window);
  style_native_dialog_children(window,
                               task_font_ == nullptr ? font_ : task_font_);
  task_list_ = GetDlgItem(window, IDC_TASK_LIST);
  ListView_SetExtendedListViewStyle(task_list_,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const auto make_column = [](const int width, const wchar_t *text) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = width;
    column.pszText = const_cast<LPWSTR>(text);
    return column;
  };
  std::array<LVCOLUMNW, 3> columns{
      make_column(145, L"Process"),
      make_column(335, L"Window"),
      make_column(80, L"PID"),
  };
  for (int index = 0; index < static_cast<int>(columns.size()); ++index)
    ListView_InsertColumn(task_list_, index,
                          &columns[static_cast<std::size_t>(index)]);
  style_list_view(task_list_, false);
  layout_tasks(window);
}

void EditorWindow::layout_tasks(const HWND window) {
  if (window == nullptr || task_list_ == nullptr)
    return;

  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const UINT window_dpi = GetDpiForWindow(window);
  const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
  const auto scaled = [dpi](const int value) { return MulDiv(value, dpi, 96); };
  const auto move = [window](const int id, const int x, const int y,
                             const int control_width,
                             const int control_height) {
    if (const HWND control = GetDlgItem(window, id); control != nullptr) {
      MoveWindow(control, x, y, std::max(1, control_width),
                 std::max(1, control_height), TRUE);
    }
  };

  const int margin = scaled(9);
  const int list_top = scaled(31);
  const int button_height = scaled(23);
  const int detail_height = scaled(18);
  const int gap = scaled(8);
  const int button_top =
      std::max(list_top + scaled(80),
               height - margin - detail_height - gap - button_height);
  const int list_width = std::max(1, width - margin * 2);
  move(IDC_TASK_LIST, margin, list_top, list_width,
       std::max(scaled(80), button_top - gap - list_top));

  int button_left = margin;
  const auto move_button = [&](const int id, const int button_width) {
    move(id, button_left, button_top, scaled(button_width), button_height);
    button_left += scaled(button_width + 7);
  };
  move_button(IDC_PICK_WINDOW, 84);
  move_button(IDC_USE_TASK, 110);
  move_button(IDC_REFRESH_TASKS, 64);
  move_button(IDC_TASK_ACTIONS, 68);
  move(IDC_TASK_DETAIL, margin, button_top + button_height + gap, list_width,
       detail_height);

  RECT list_client{};
  GetClientRect(task_list_, &list_client);
  const int scrollbar =
      GetSystemMetricsForDpi(SM_CXVSCROLL, static_cast<UINT>(dpi));
  const int available = std::max(
      scaled(300), static_cast<int>(list_client.right) - scrollbar - scaled(3));
  const int pid_width = std::clamp(scaled(62), scaled(54), available / 4);
  const int process_width =
      std::clamp(available * 30 / 100, scaled(125), scaled(230));
  const int window_width =
      std::max(scaled(130), available - process_width - pid_width);
  ListView_SetColumnWidth(task_list_, 0, process_width);
  ListView_SetColumnWidth(task_list_, 1, window_width);
  ListView_SetColumnWidth(
      task_list_, 2,
      std::max(scaled(54), available - process_width - window_width));
}

void EditorWindow::refresh_tasks() {
  if (task_list_ == nullptr)
    return;
  const HWND previous = selected_task_window();
  task_candidates_.clear();
  ListView_DeleteAllItems(task_list_);
  auto matching_profile = profile_;
  matching_profile.enabled = true;
  for (auto candidate : layerc::platform::enumerate_attachable_windows()) {
    if (candidate.process_id == GetCurrentProcessId())
      continue;
    if ((awaiting_edit_target_ || awaiting_calibration_target_) &&
        !matching_profile.game.global &&
        !layerc::platform::profile_matches_window(candidate, matching_profile))
      continue;
    task_candidates_.push_back(std::move(candidate));
    const auto &item = task_candidates_.back();
    const int row = static_cast<int>(task_candidates_.size() - 1U);
    LVITEMW list_item{};
    list_item.mask = LVIF_TEXT | LVIF_PARAM;
    list_item.iItem = row;
    list_item.pszText = const_cast<LPWSTR>(item.executable_name.c_str());
    list_item.lParam = static_cast<LPARAM>(row);
    ListView_InsertItem(task_list_, &list_item);
    ListView_SetItemText(task_list_, row, 1,
                         const_cast<LPWSTR>(item.title.empty()
                                                ? L"(untitled)"
                                                : item.title.c_str()));
    const auto pid = std::to_wstring(item.process_id);
    ListView_SetItemText(task_list_, row, 2, const_cast<LPWSTR>(pid.c_str()));
    if (item.window == previous)
      ListView_SetItemState(task_list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
  }
  update_task_detail();
}

HWND EditorWindow::selected_task_window() const {
  if (task_list_ == nullptr)
    return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
             ? task_candidates_[static_cast<std::size_t>(row)].window
             : nullptr;
}

const layerc::platform::WindowCandidate *EditorWindow::selected_task() const {
  if (task_list_ == nullptr)
    return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
             ? &task_candidates_[static_cast<std::size_t>(row)]
             : nullptr;
}

const layerc::platform::WindowCandidate *
EditorWindow::require_selected_task(const bool live_window_required) {
  const auto *candidate = selected_task();
  if (candidate != nullptr &&
      (!live_window_required || IsWindow(candidate->window)))
    return candidate;
  MessageBoxW(task_dialog_,
              candidate == nullptr
                  ? L"Select a task first."
                  : L"The selected task has closed. Refresh the list before "
                    L"using a live-window action.",
              L"LayerC Editor", MB_OK | MB_ICONINFORMATION);
  return nullptr;
}

void EditorWindow::update_task_detail() {
  if (task_dialog_ == nullptr)
    return;
  const auto *candidate = selected_task();
  const BOOL enabled = candidate == nullptr ? FALSE : TRUE;
  EnableWindow(GetDlgItem(task_dialog_, IDC_USE_TASK), enabled);
  EnableWindow(GetDlgItem(task_dialog_, IDC_TASK_ACTIONS), enabled);
  set_text(GetDlgItem(task_dialog_, IDC_TASK_DETAIL),
           candidate == nullptr ? L"Select a task to see its executable path."
                                : candidate->executable_path);
}

void EditorWindow::apply_task(
    const layerc::platform::WindowCandidate &candidate) {
  sync_profile_from_controls();
  profile_.game.global = false;
  const bool exact_path = owner_draw_checkbox_checked(
      GetDlgItem(dialog_, IDC_APPLICATION_EXACT_PATH));
  profile_.game.executable_path =
      exact_path ? to_utf8(candidate.executable_path) : std::string{};
  profile_.game.executable_name = to_utf8(candidate.executable_name);
  profile_.game.window_class.clear();
  profile_.game.window_title.clear();
  if (profile_.name == "New Profile" || profile_.name.empty()) {
    const auto suggested =
        std::filesystem::path(candidate.executable_name).stem().wstring();
    profile_.name = to_utf8(suggested);
    profile_.id = safe_identifier(suggested);
  }
  refresh_profile_controls();
  record_profile_change(ChangeKind::immediate);
  update_task_detail();
  set_status(current_path_.empty()
                 ? L"Application selected — Save once to create the profile."
                 : exact_path
                       ? L"Application selected — matching its exact path."
                       : L"Application selected — matching by executable name.");
}

void EditorWindow::use_selected_task() {
  // Attaching stores the enumerated executable matcher and does not require
  // the original HWND to remain alive between enumeration and this click.
  const auto *candidate = require_selected_task(false);
  if (candidate == nullptr)
    return;
  apply_task(*candidate);
}

void EditorWindow::show_tasks() {
  if (task_dialog_ == nullptr || !IsWindow(task_dialog_)) {
    task_dialog_ = CreateDialogParamW(
        instance_, MAKEINTRESOURCEW(IDD_TASKS), dialog_,
        &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_tasks,
                                &EditorWindow::handle_task_message>,
        reinterpret_cast<LPARAM>(this));
  }
  if (task_dialog_ == nullptr) {
    set_status(L"Could not open the task picker.");
    return;
  }
  refresh_tasks();
  set_dark_title_bar(task_dialog_);
  (void)layerc::platform::apply_capture_exclusion(task_dialog_,
                                                  capture_exclusion_);
  ShowWindow(task_dialog_, SW_SHOWNORMAL);
  SetForegroundWindow(task_dialog_);
}

void EditorWindow::begin_picker() {
  if (picking_) {
    cancel_picker();
    return;
  }
  picking_ = true;
  task_was_visible_ = task_dialog_ != nullptr && IsWindowVisible(task_dialog_);
  if (task_was_visible_)
    ShowWindow(task_dialog_, SW_HIDE);
  SetFocus(dialog_);
  SetCapture(dialog_);
  set_status(
      L"Pick Window active — click a game window, or press Esc to cancel.");
}

void EditorWindow::finish_picker() {
  POINT cursor{};
  GetCursorPos(&cursor);
  const auto candidate = layerc::platform::window_at_point(cursor);
  cancel_picker();
  if (!candidate.has_value() ||
      candidate->process_id == GetCurrentProcessId()) {
    set_status(L"No eligible window was picked.");
    return;
  }
  if (awaiting_calibration_target_ || awaiting_edit_target_) {
    auto matching = profile_;
    matching.enabled = true;
    if (!matching.game.global &&
        !layerc::platform::profile_matches_window(*candidate, matching)) {
      set_status(L"The picked window does not match this profile.");
      return;
    }
    if (awaiting_calibration_target_) {
      awaiting_calibration_target_ = false;
      launch_calibration(candidate->window);
    } else {
      awaiting_edit_target_ = false;
      begin_edit_on_window(candidate->window);
    }
    return;
  }
  apply_task(*candidate);
}

void EditorWindow::cancel_picker() {
  if (!picking_)
    return;
  picking_ = false;
  if (GetCapture() == dialog_)
    ReleaseCapture();
  if (task_was_visible_ && task_dialog_ != nullptr)
    ShowWindow(task_dialog_, SW_SHOWNORMAL);
  task_was_visible_ = false;
}

LRESULT EditorWindow::handle_task_message(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDC_PICK_WINDOW:
      begin_picker();
      return TRUE;
    case IDC_USE_TASK:
      use_selected_task();
      return TRUE;
    case IDC_REFRESH_TASKS:
      refresh_tasks();
      return TRUE;
    case IDC_TASK_ACTIONS:
      show_task_actions_menu(window);
      return TRUE;
    case IDC_EDIT_SELECTED_TASK:
      edit_selected_task();
      return TRUE;
    case IDC_CALIBRATE_SELECTED_TASK:
      calibrate_selected_task();
      return TRUE;
    default:
      break;
    }
    break;
  case WM_SIZE:
    layout_tasks(window);
    return TRUE;
  case WM_DPICHANGED: {
    apply_suggested_dpi_rect(window, lparam);
    const HFONT replacement = create_editor_font(HIWORD(wparam));
    if (replacement != nullptr) {
      const HFONT previous = task_font_;
      task_font_ = replacement;
      style_native_dialog_children(window, task_font_);
      if (previous != nullptr)
        DeleteObject(previous);
    }
    layout_tasks(window);
    return TRUE;
  }
  case WM_GETMINMAXINFO: {
    const UINT window_dpi = GetDpiForWindow(window);
    const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
    auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
    limits->ptMinTrackSize = {MulDiv(520, dpi, 96), MulDiv(260, dpi, 96)};
    return TRUE;
  }
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_TASK_LIST) {
      if (header->code == LVN_GETEMPTYMARKUP) {
        auto *markup = reinterpret_cast<NMLVEMPTYMARKUP *>(lparam);
        markup->dwFlags = EMF_CENTERED;
        lstrcpynW(markup->szMarkup, L"No eligible windows detected.",
                  static_cast<int>(sizeof(markup->szMarkup) /
                                   sizeof(markup->szMarkup[0])));
        return TRUE;
      }
      if (header->code == LVN_ITEMCHANGED)
        update_task_detail();
      else if (header->code == NM_DBLCLK) {
        if (awaiting_calibration_target_)
          calibrate_selected_task();
        else if (awaiting_edit_target_)
          edit_selected_task();
        else
          use_selected_task();
      } else if (header->code == NM_CUSTOMDRAW) {
        auto *draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
        const auto list_colors =
            colors(layerc::platform::EditorControlRole::list);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
          return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          draw->clrText = list_colors.text;
          draw->clrTextBk = (draw->nmcd.uItemState & CDIS_SELECTED) != 0
                                ? list_colors.selection
                                : list_colors.fill;
          return CDRF_NEWFONT;
        }
      }
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_BUTTON) {
      draw_button(*draw, layerc::platform::EditorControlRole::button);
      return TRUE;
    }
    break;
  }
  case WM_CLOSE:
    DestroyWindow(window);
    return TRUE;
  case WM_DESTROY:
    if (task_dialog_ == window) {
      task_dialog_ = nullptr;
      task_list_ = nullptr;
      awaiting_edit_target_ = false;
      awaiting_calibration_target_ = false;
      if (task_font_ != nullptr) {
        DeleteObject(task_font_);
        task_font_ = nullptr;
      }
    }
    return TRUE;
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
