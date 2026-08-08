#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <utility>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
INT_PTR CALLBACK EditorWindow::task_dialog_proc(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize_tasks(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_task_message(window, message, wparam, lparam);
}


void EditorWindow::initialize_tasks(const HWND window) {
  task_dialog_ = window;
  const UINT window_dpi = GetDpiForWindow(window);
  const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
  task_font_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Segoe UI");
  set_dark_title_bar(window);
  style_native_dialog_children(window,
                               task_font_ == nullptr ? font_ : task_font_);
  task_list_ = GetDlgItem(window, IDC_TASK_LIST);
  ListView_SetExtendedListViewStyle(task_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const std::array<LVCOLUMNW, 3> columns{{
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 145, .pszText = const_cast<LPWSTR>(L"Process")},
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 335, .pszText = const_cast<LPWSTR>(L"Window")},
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 80, .pszText = const_cast<LPWSTR>(L"PID")},
  }};
  for (int index = 0; index < static_cast<int>(columns.size()); ++index)
    ListView_InsertColumn(task_list_, index, const_cast<LVCOLUMNW *>(&columns[static_cast<std::size_t>(index)]));
  style_list_view(task_list_, false);
  layout_tasks(window);
}

void EditorWindow::layout_tasks(const HWND window) {
  if (window == nullptr || task_list_ == nullptr) return;

  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const UINT window_dpi = GetDpiForWindow(window);
  const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
  const auto scaled = [dpi](const int value) {
    return MulDiv(value, dpi, 96);
  };
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
  move_button(IDC_EDIT_SELECTED_TASK, 102);
  move(IDC_TASK_DETAIL, margin, button_top + button_height + gap, list_width,
       detail_height);

  RECT list_client{};
  GetClientRect(task_list_, &list_client);
  const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL,
                                                static_cast<UINT>(dpi));
  const int available =
      std::max(scaled(300), static_cast<int>(list_client.right) -
                                   scrollbar - scaled(3));
  const int pid_width = std::clamp(scaled(62), scaled(54), available / 4);
  const int process_width =
      std::clamp(available * 30 / 100, scaled(125), scaled(230));
  const int window_width =
      std::max(scaled(130), available - process_width - pid_width);
  ListView_SetColumnWidth(task_list_, 0, process_width);
  ListView_SetColumnWidth(task_list_, 1, window_width);
  ListView_SetColumnWidth(task_list_, 2,
                          std::max(scaled(54),
                                   available - process_width - window_width));
}

void EditorWindow::refresh_tasks() {
  if (task_list_ == nullptr) return;
  const HWND previous = selected_task_window();
  task_candidates_.clear(); ListView_DeleteAllItems(task_list_);
  auto matching_profile = profile_;
  matching_profile.enabled = true;
  for (auto candidate : layerc::platform::enumerate_attachable_windows()) {
    if (candidate.process_id == GetCurrentProcessId()) continue;
    if (awaiting_edit_target_ && !matching_profile.game.global &&
        !layerc::platform::profile_matches_window(candidate,
                                                  matching_profile))
      continue;
    task_candidates_.push_back(std::move(candidate));
    const auto &item = task_candidates_.back();
    const int row = static_cast<int>(task_candidates_.size() - 1U);
    LVITEMW list_item{.mask = LVIF_TEXT | LVIF_PARAM, .iItem = row,
                      .pszText = const_cast<LPWSTR>(item.executable_name.c_str()), .lParam = static_cast<LPARAM>(row)};
    ListView_InsertItem(task_list_, &list_item);
    ListView_SetItemText(task_list_, row, 1, const_cast<LPWSTR>(item.title.empty() ? L"(untitled)" : item.title.c_str()));
    const auto pid = std::to_wstring(item.process_id);
    ListView_SetItemText(task_list_, row, 2, const_cast<LPWSTR>(pid.c_str()));
    if (item.window == previous) ListView_SetItemState(task_list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                                                        LVIS_SELECTED | LVIS_FOCUSED);
  }
  update_task_detail();
}

HWND EditorWindow::selected_task_window() const {
  if (task_list_ == nullptr) return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
      ? task_candidates_[static_cast<std::size_t>(row)].window : nullptr;
}

const layerc::platform::WindowCandidate *EditorWindow::selected_task() const {
  if (task_list_ == nullptr) return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
      ? &task_candidates_[static_cast<std::size_t>(row)] : nullptr;
}

void EditorWindow::update_task_detail() {
  if (task_dialog_ == nullptr) return;
  const auto *candidate = selected_task();
  set_text(GetDlgItem(task_dialog_, IDC_TASK_DETAIL), candidate == nullptr
      ? L"Select a task to see its executable path." : candidate->executable_path);
}

void EditorWindow::apply_task(const layerc::platform::WindowCandidate &candidate) {
  sync_profile_from_controls();
  profile_.game.global = false;
  profile_.game.executable_path = to_utf8(candidate.executable_path);
  profile_.game.executable_name = to_utf8(candidate.executable_name);
  profile_.game.window_class.clear(); profile_.game.window_title.clear();
  if (profile_.name == "New Profile" || profile_.name.empty()) {
    const auto suggested = std::filesystem::path(candidate.executable_name).stem().wstring();
    profile_.name = to_utf8(suggested); profile_.id = safe_identifier(suggested);
  }
  refresh_profile_controls();
  record_profile_change(ChangeKind::immediate);
  update_task_detail();
  set_status(current_path_.empty()
                 ? L"Task selected — Save once to create the profile."
                 : L"Task selected — autosaving the durable matcher.");
}

void EditorWindow::use_selected_task() {
  const auto *candidate = selected_task();
  if (candidate == nullptr) {
    MessageBoxW(task_dialog_, L"Select a visible task first.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  apply_task(*candidate);
}

void EditorWindow::show_tasks() {
  if (task_dialog_ == nullptr || !IsWindow(task_dialog_)) {
    task_dialog_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_TASKS), dialog_,
                                      &EditorWindow::task_dialog_proc, reinterpret_cast<LPARAM>(this));
  }
  if (task_dialog_ == nullptr) { set_status(L"Could not open the task picker."); return; }
  refresh_tasks(); set_dark_title_bar(task_dialog_);
  (void)layerc::platform::apply_capture_exclusion(task_dialog_, capture_exclusion_);
  ShowWindow(task_dialog_, SW_SHOWNORMAL); SetForegroundWindow(task_dialog_);
}

void EditorWindow::begin_picker() {
  if (picking_) { cancel_picker(); return; }
  picking_ = true;
  task_was_visible_ = task_dialog_ != nullptr && IsWindowVisible(task_dialog_);
  if (task_was_visible_) ShowWindow(task_dialog_, SW_HIDE);
  SetFocus(dialog_);
  SetCapture(dialog_);
  set_status(L"Pick Window active — click a game window, or press Esc to cancel.");
}

void EditorWindow::finish_picker() {
  POINT cursor{}; GetCursorPos(&cursor);
  const auto candidate = layerc::platform::window_at_point(cursor);
  cancel_picker();
  if (!candidate.has_value() ||
      candidate->process_id == GetCurrentProcessId()) {
    set_status(L"No eligible window was picked.");
    return;
  }
  apply_task(*candidate);
}

void EditorWindow::cancel_picker() {
  if (!picking_) return;
  picking_ = false;
  if (GetCapture() == dialog_) ReleaseCapture();
  if (task_was_visible_ && task_dialog_ != nullptr) ShowWindow(task_dialog_, SW_SHOWNORMAL);
  task_was_visible_ = false;
}

LRESULT EditorWindow::handle_task_message(const HWND window, const UINT message,
                                          const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDC_PICK_WINDOW: begin_picker(); return TRUE;
    case IDC_USE_TASK: use_selected_task(); return TRUE;
    case IDC_REFRESH_TASKS: refresh_tasks(); return TRUE;
    case IDC_EDIT_SELECTED_TASK: edit_selected_task(); return TRUE;
    default: break;
    }
    break;
  case WM_SIZE:
    layout_tasks(window);
    return TRUE;
  case WM_DPICHANGED: {
    const auto *suggested = reinterpret_cast<const RECT *>(lparam);
    SetWindowPos(window, nullptr, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    const int dpi = static_cast<int>(HIWORD(wparam));
    const HFONT replacement =
        CreateFontW(-MulDiv(9, dpi == 0 ? 96 : dpi, 72), 0, 0, 0,
                    FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    L"Segoe UI");
    if (replacement != nullptr) {
      const HFONT previous = task_font_;
      task_font_ = replacement;
      style_native_dialog_children(window, task_font_);
      if (previous != nullptr) DeleteObject(previous);
    }
    layout_tasks(window);
    return TRUE;
  }
  case WM_GETMINMAXINFO: {
    const UINT window_dpi = GetDpiForWindow(window);
    const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
    auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
    limits->ptMinTrackSize = {MulDiv(520, dpi, 96),
                              MulDiv(260, dpi, 96)};
    return TRUE;
  }
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_TASK_LIST) {
      if (header->code == LVN_ITEMCHANGED) update_task_detail();
      else if (header->code == NM_DBLCLK) {
        if (awaiting_edit_target_) edit_selected_task();
        else use_selected_task();
      }
      else if (header->code == NM_CUSTOMDRAW) {
        auto *draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
        const auto list_colors = colors(layerc::platform::EditorControlRole::list);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          draw->clrText = list_colors.text;
          draw->clrTextBk = (draw->nmcd.uItemState & CDIS_SELECTED) != 0 ? list_colors.selection : list_colors.fill;
          return CDRF_NEWFONT;
        }
      }
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_BUTTON) { draw_button(*draw, layerc::platform::EditorControlRole::button); return TRUE; }
    break;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: { RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wparam), &client,
    brush(layerc::platform::EditorControlRole::window)); return TRUE; }
  case WM_CLOSE: DestroyWindow(window); return TRUE;
  case WM_DESTROY:
    if (task_dialog_ == window) {
      task_dialog_ = nullptr;
      task_list_ = nullptr;
      awaiting_edit_target_ = false;
      if (task_font_ != nullptr) {
        DeleteObject(task_font_);
        task_font_ = nullptr;
      }
    }
    return TRUE;
  default: break;
  }
  return FALSE;
}

} // namespace layerc::editor
