#include "editor_app.hpp"
#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <optional>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
int EditorWindow::run(const HINSTANCE instance, const int show_command) {
  instance_ = instance;
  enable_native_dark_mode();
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  dialog_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_EDITOR), nullptr,
                               &EditorWindow::dialog_proc,
                               reinterpret_cast<LPARAM>(this));
  if (dialog_ == nullptr) return 1;
  // CW_USEDEFAULT only has meaning when a window is created.  Passing it to
  // SetWindowPos placed the dialog at (-2147483648, -2147483648), leaving a
  // running but invisible editor.  Center it inside the primary work area
  // instead, while keeping the compact fixed initial size.
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int width = std::min(1210, std::max(640, work_width));
  const int height = std::min(760, std::max(480, work_height));
  const int left = static_cast<int>(work.left) + std::max(0, (work_width - width) / 2);
  const int top = static_cast<int>(work.top) + std::max(0, (work_height - height) / 2);
  SetWindowPos(dialog_, nullptr, left, top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(dialog_, show_command);
  UpdateWindow(dialog_);
  const HACCEL accelerators =
      LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_EDITOR_ACCELERATORS));
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (handle_binding_capture_message(message)) continue;
    if (accelerators != nullptr &&
        TranslateAcceleratorW(dialog_, accelerators, &message) != 0) continue;
    const bool main_handled = IsDialogMessageW(dialog_, &message) != FALSE;
    const bool task_handled = task_dialog_ != nullptr && IsWindow(task_dialog_) &&
                              IsDialogMessageW(task_dialog_, &message) != FALSE;
    if (!main_handled && !task_handled) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

INT_PTR CALLBACK EditorWindow::dialog_proc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_main_message(window, message, wparam, lparam);
}


void EditorWindow::initialize(const HWND window) {
  dialog_ = window;
  const UINT window_dpi = GetDpiForWindow(window);
  const int font_height = -MulDiv(9, window_dpi == 0 ? 96 : static_cast<int>(window_dpi), 72);
  font_ = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  theme_ = layerc::platform::editor_theme();
  accent_ = layerc::platform::editor_accent();
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  set_dark_title_bar(dialog_);
  (void)layerc::platform::apply_capture_exclusion(dialog_, capture_exclusion_);
  style_dialog_children(dialog_);
  rebuild_brushes();
  profile_list_ = GetDlgItem(dialog_, IDC_PROFILE_LIST);
  cooldown_list_ = GetDlgItem(dialog_, IDC_COOLDOWN_LIST);
  bar_list_ = GetDlgItem(dialog_, IDC_BAR_LIST);
  preview_ = GetDlgItem(dialog_, IDC_PREVIEW);
  status_ = GetDlgItem(dialog_, IDC_STATUS);
  application_path_ = GetDlgItem(dialog_, IDC_APPLICATION_PATH);
  ListView_SetExtendedListViewStyle(profile_list_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                                   LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  LVCOLUMNW column{.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 198,
                   .pszText = const_cast<LPWSTR>(L"Profile")};
  ListView_InsertColumn(profile_list_, 0, &column);
  style_list_view(profile_list_, true);
  rebuild_profile_check_images();
  DrawMenuBar(dialog_);
  if (preview_ != nullptr) {
    const auto style = GetWindowLongPtrW(preview_, GWL_STYLE);
    SetWindowLongPtrW(preview_, GWL_STYLE, style | SS_NOTIFY);
    SetWindowPos(preview_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowSubclass(preview_, &EditorWindow::preview_subclass, 1,
                      reinterpret_cast<DWORD_PTR>(this));
  }
  layout();
  reload_profile_list({}, true);
  set_status(L"Ready");
}


LRESULT EditorWindow::handle_main_message(const HWND window, const UINT message,
                                          const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_SIZE: layout(); return TRUE;
  case WM_GETMINMAXINFO:
    reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {1080, 700}; return TRUE;
  case WM_COMMAND: {
    const int id = LOWORD(wparam); const auto notification = HIWORD(wparam);
    if (id == IDC_PROFILE_NAME || id == IDC_PROFILE_ID || id == IDC_BAR_WIDTH || id == IDC_BAR_HEIGHT) {
      if (!syncing_ && notification == EN_CHANGE) {
        sync_profile_from_controls();
        dirty_ = true;
        InvalidateRect(preview_, nullptr, TRUE);
      }
      return TRUE;
    }
    if ((id == IDC_SHOW_TEXT || id == IDC_HIDE_READY) &&
        notification == BN_CLICKED) {
      toggle_owner_draw_checkbox(GetDlgItem(dialog_, id));
      sync_profile_from_controls();
      dirty_ = true;
      InvalidateRect(GetDlgItem(dialog_, id), nullptr, TRUE);
      InvalidateRect(preview_, nullptr, TRUE);
      return TRUE;
    }
    switch (id) {
    case IDC_FILE_MENU: show_file_menu(); return TRUE;
    case IDM_FILE_NEW: new_profile(); return TRUE;
    case IDM_FILE_SAVE: (void)save_profile(false); return TRUE;
    case IDM_FILE_SAVE_AS: (void)save_profile(true); return TRUE;
    case IDC_ATTACH_LEFT: attach_profile(); return TRUE;
    case IDC_SHOW_TASKS_LEFT: show_tasks(); return TRUE;
    case IDM_FILE_OPTIONS: open_options(); return TRUE;
    case IDM_FILE_SPLIT_BARS: split_independent_bars(); return TRUE;
    case IDC_ADD_COOLDOWN: add_cooldown(); return TRUE;
    case IDC_REMOVE_COOLDOWN: remove_cooldown(); return TRUE;
    case IDC_CAPTURE_BINDING: begin_binding_capture(); return TRUE;
    case IDC_BAR_FILL: case IDC_BAR_BACKGROUND: case IDC_BAR_BORDER: case IDC_BAR_TEXT: choose_bar_color(id); return TRUE;
    case IDM_FILE_DELETE: delete_profile(); return TRUE;
    case IDM_FILE_OPEN_FOLDER: open_profiles_folder(); return TRUE;
    case IDM_FILE_EXIT: SendMessageW(dialog_, WM_CLOSE, 0, 0); return TRUE;
    default: break;
    }
    if (id == IDC_COOLDOWN_LIST && notification == LBN_SELCHANGE) {
      cancel_binding_capture();
      refresh_binding_controls();
      return TRUE;
    }
    if (id == IDC_BAR_LIST && notification == LBN_SELCHANGE) {
      sync_profile_from_controls();
      refresh_bar_controls();
      return TRUE;
    }
    break;
  }
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_PROFILE_LIST) {
      if (header->code == NM_CLICK) {
        const auto *click = reinterpret_cast<const NMITEMACTIVATE *>(lparam);
        LVHITTESTINFO hit{};
        hit.pt = click->ptAction;
        const int row = ListView_HitTest(profile_list_, &hit);
        if (row >= 0 && (hit.flags & LVHT_ONITEMSTATEICON) != 0 &&
            static_cast<std::size_t>(row) < entries_.size()) {
          const bool enabled = !entries_[static_cast<std::size_t>(row)].enabled;
          const bool was_syncing = syncing_;
          syncing_ = true;
          ListView_SetCheckState(profile_list_, row, enabled);
          syncing_ = was_syncing;
          save_profile_enabled(row, enabled);
          return TRUE;
        }
      } else if (header->code == LVN_ITEMCHANGED) {
        const auto *change = reinterpret_cast<const NMLISTVIEW *>(lparam);
        if (syncing_) return TRUE;
        if ((change->uChanged & LVIF_STATE) != 0 &&
            ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) != 0) {
          save_profile_enabled(change->iItem, ListView_GetCheckState(profile_list_, change->iItem) != FALSE);
          return TRUE;
        }
        if ((change->uChanged & LVIF_STATE) != 0 && (change->uNewState & LVIS_SELECTED) != 0 &&
            (change->uOldState & LVIS_SELECTED) == 0) { open_profile_row(change->iItem, true); return TRUE; }
      }
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTVIEW && draw->CtlID == IDC_PROFILE_LIST) {
      draw_profile_item(*draw); return TRUE;
    }
    if (draw->CtlType == ODT_LISTBOX) { draw_list_box_item(*draw); return TRUE; }
    if (draw->CtlType != ODT_BUTTON) break;
    const int id = static_cast<int>(draw->CtlID);
    if (id == IDC_SHOW_TEXT || id == IDC_HIDE_READY) {
      draw_checkbox(*draw); return TRUE;
    }
    std::optional<COLORREF> overlay_color;
    if (const auto *panel = selected_bar()) {
      if (id == IDC_BAR_FILL) overlay_color = color_ref(panel->bar.fill_color);
      else if (id == IDC_BAR_BACKGROUND) overlay_color = color_ref(panel->bar.background_color);
      else if (id == IDC_BAR_BORDER) overlay_color = color_ref(panel->bar.border_color);
      else if (id == IDC_BAR_TEXT) overlay_color = color_ref(panel->bar.text_color);
    }
    const bool toolbar = id == IDC_FILE_MENU;
    draw_button(*draw, toolbar ? layerc::platform::EditorControlRole::toolbar
                               : layerc::platform::EditorControlRole::button, overlay_color);
    return TRUE;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: {
    RECT client{}; GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client, brush(layerc::platform::EditorControlRole::window)); return TRUE;
  }
  case WM_LBUTTONUP:
    if (picking_) { finish_picker(); return TRUE; }
    break;
  case WM_KEYDOWN:
    if (picking_ && wparam == VK_ESCAPE) { cancel_picker(); set_status(L"Window picker cancelled."); return TRUE; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
      if (wparam == 'S') { (void)save_profile(false); return TRUE; }
      if (wparam == 'N') { new_profile(); return TRUE; }
    }
    break;
  case WM_CAPTURECHANGED:
    if (picking_) cancel_picker();
    break;
  case WM_CLOSE:
    if (confirm_unsaved_changes()) DestroyWindow(window);
    return TRUE;
  case WM_DESTROY:
    cancel_picker();
    if (task_dialog_ != nullptr && IsWindow(task_dialog_)) DestroyWindow(task_dialog_);
    task_dialog_ = nullptr;
    if (profile_check_images_ != nullptr) {
      ImageList_Destroy(profile_check_images_);
      profile_check_images_ = nullptr;
    }
    for (auto &brush_value : brushes_) { if (brush_value != nullptr) DeleteObject(brush_value); brush_value = nullptr; }
    if (font_ != nullptr) { DeleteObject(font_); font_ = nullptr; }
    PostQuitMessage(0); return TRUE;
  default: break;
  }
  return FALSE;
}

int EditorApp::run(const HINSTANCE instance, const int show_command) {
  EditorWindow editor;
  return editor.run(instance, show_command);
}

} // namespace layerc::editor
