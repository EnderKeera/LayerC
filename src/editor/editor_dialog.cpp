#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_app.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_window.hpp"

#include "editor_resource.h"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/editor_appearance.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_error.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
namespace {

std::wstring editor_single_instance_name() {
  HANDLE raw_token{};
  TOKEN_STATISTICS statistics{};
  DWORD required{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE) {
    const layerc::platform::OwnedHandle token(raw_token);
    if (GetTokenInformation(token.get(), TokenStatistics, &statistics,
                            sizeof(statistics), &required) != FALSE) {
      wchar_t name[96]{};
      swprintf_s(
          name, L"Local\\LayerC.LayerE.%08lX%08lX.v1",
          static_cast<unsigned long>(statistics.AuthenticationId.HighPart),
          static_cast<unsigned long>(statistics.AuthenticationId.LowPart));
      return name;
    }
  }
  return L"Local\\LayerC.LayerE.SingleInstance.v1";
}

// "#32770" is the shared system dialog class: FindWindowW alone would match
// any application's dialog titled LayerC Editor. Only raise a window owned by
// a process from this install.
void raise_existing_editor() {
  struct Context {
    HWND found{};
  } context;
  EnumWindows(
      [](const HWND window, const LPARAM raw) -> BOOL {
        auto &context = *reinterpret_cast<Context *>(raw);
        if (GetWindow(window, GW_OWNER) != nullptr ||
            layerc::platform::window_class(window) != L"#32770" ||
            layerc::platform::window_text(window) != L"LayerC Editor")
          return TRUE;
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        if (process_id == 0 || !layerc::platform::is_sibling_process_image(
                                   process_id, L"layere.exe"))
          return TRUE;
        context.found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&context));
  if (context.found == nullptr)
    return;
  if (IsIconic(context.found))
    ShowWindow(context.found, SW_RESTORE);
  ShowWindow(context.found, SW_SHOW);
  SetForegroundWindow(context.found);
}

} // namespace

int EditorWindow::run(const HINSTANCE instance, const int show_command) {
  shell_.instance = instance;
  const auto migration = layerc::platform::migrate_editor_appearance();
  const auto appearance = layerc::platform::read_editor_appearance();
  if (appearance.value)
    appearance_editor_.draft().begin(*appearance.value);
  const std::uint32_t appearance_error =
      migration.error != 0 ? migration.error : appearance.error;
  if (appearance_error != 0) {
    const auto detail = layerc::platform::windows_error_text(appearance_error);
    MessageBoxW(nullptr,
                (L"LayerC could not read or migrate editor appearance settings. " +
                 detail)
                    .c_str(),
                L"LayerC settings", MB_OK | MB_ICONWARNING);
  }
  const auto capture = layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::capture_exclusion);
  if (capture.value)
    shell_.capture_exclusion = *capture.value;
  if (capture.failed()) {
    const auto detail = layerc::platform::windows_error_text(capture.error);
    MessageBoxW(nullptr,
                (L"LayerC could not read the capture-exclusion setting. " +
                 detail)
                    .c_str(),
                L"LayerC settings", MB_OK | MB_ICONWARNING);
  }
  const auto recovered = layerc::platform::recover_profile_transactions(
      layerc::platform::profile_directory());
  if (const auto error = recovered.error(); error.has_value()) {
    const auto message =
        L"LayerC Editor could not recover an interrupted profile import (" +
        layerc::platform::utf8_to_wide(
            layerc::platform::profile_write_error_text(*error)) +
        L"). Resolve the _transactions directory before editing profiles.";
    MessageBoxW(nullptr, message.c_str(), L"LayerC profile recovery",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  enable_native_dark_mode();
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC =
                                    ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  shell_.window = CreateDialogParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_EDITOR), nullptr,
      &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize,
                              &EditorWindow::handle_main_message>,
      reinterpret_cast<LPARAM>(this));
  if (shell_.window == nullptr)
    return 1;
  // CW_USEDEFAULT only has meaning when a window is created.  Passing it to
  // SetWindowPos placed the dialog at (-2147483648, -2147483648), leaving a
  // running but invisible editor. Center it inside the nearest monitor's work
  // area instead, while keeping the compact initial size.
  MONITORINFO initial_monitor{};
  initial_monitor.cbSize = sizeof(initial_monitor);
  RECT work{};
  if (GetMonitorInfoW(MonitorFromWindow(shell_.window, MONITOR_DEFAULTTONEAREST),
                      &initial_monitor) != FALSE)
    work = initial_monitor.rcWork;
  else
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int dpi = window_base_dpi(shell_.window);
  const int width = std::max(1, std::min(dpi_scaled(1210, dpi), work_width));
  const int height = std::max(1, std::min(dpi_scaled(760, dpi), work_height));
  const int left =
      static_cast<int>(work.left) + std::max(0, (work_width - width) / 2);
  const int top =
      static_cast<int>(work.top) + std::max(0, (work_height - height) / 2);
  SetWindowPos(shell_.window, nullptr, left, top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(shell_.window, show_command);
  UpdateWindow(shell_.window);
  const HACCEL accelerators =
      LoadAcceleratorsW(shell_.instance, MAKEINTRESOURCEW(IDR_EDITOR_ACCELERATORS));
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (handle_binding_capture_message(message))
      continue;
    if (task_picker_.picking() && message.message == WM_KEYDOWN &&
        message.wParam == VK_ESCAPE) {
      cancel_picker();
      set_status(L"Window picker cancelled.");
      continue;
    }
    if (accelerators != nullptr && IsWindowEnabled(shell_.window) != FALSE &&
        TranslateAcceleratorW(shell_.window, accelerators, &message) != 0)
      continue;
    const bool main_handled = IsDialogMessageW(shell_.window, &message) != FALSE;
    const bool task_handled = task_picker_.window != nullptr &&
                              IsWindow(task_picker_.window) &&
                              IsDialogMessageW(task_picker_.window, &message) != FALSE;
    if (!main_handled && !task_handled) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

void EditorWindow::initialize(const HWND window) {
  shell_.window = window;
  shell_.font = layerc::platform::create_native_dialog_font(GetDpiForWindow(window));
  appearance_editor_.set_appearance(appearance_editor_.draft().staged().theme,
                                    appearance_editor_.draft().staged().accent);
  apply_dialog_chrome(shell_.window, shell_.capture_exclusion);
  style_dialog_children(shell_.window);
  rebuild_brushes();
  shell_.profile_list = GetDlgItem(shell_.window, IDC_PROFILE_LIST);
  cooldown_editor_.list = GetDlgItem(shell_.window, IDC_COOLDOWN_LIST);
  panel_preview_.list = GetDlgItem(shell_.window, IDC_BAR_LIST);
  panel_preview_.window = GetDlgItem(shell_.window, IDC_PREVIEW);
  shell_.status = GetDlgItem(shell_.window, IDC_STATUS);
  shell_.application_path = GetDlgItem(shell_.window, IDC_APPLICATION_PATH);
  shell_.advanced_visible = false;
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_ADVANCED), false);
  for (const wchar_t *label : {L"Ignore", L"Restart"})
    SendMessageW(GetDlgItem(shell_.window, IDC_COOLDOWN_RETRIGGER), CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  for (const wchar_t *label : {L"Single", L"Sequential", L"Parallel"})
    SendMessageW(GetDlgItem(shell_.window, IDC_COOLDOWN_RECOVERY), CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  ListView_SetExtendedListViewStyle(shell_.profile_list,
                                    LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                        LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH;
  column.cx = 198;
  column.pszText = const_cast<LPWSTR>(L"Profile");
  ListView_InsertColumn(shell_.profile_list, 0, &column);
  style_list_view(shell_.profile_list, true);
  rebuild_profile_check_hit_images();
  DrawMenuBar(shell_.window);
  if (panel_preview_.window != nullptr) {
    const auto style = GetWindowLongPtrW(panel_preview_.window, GWL_STYLE);
    SetWindowLongPtrW(panel_preview_.window, GWL_STYLE, style | SS_NOTIFY);
    SetWindowPos(panel_preview_.window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    SetWindowSubclass(panel_preview_.window, &EditorWindow::preview_subclass, 1,
                      reinterpret_cast<DWORD_PTR>(this));
  }
  layout();
  reload_profile_list({}, true);
  live_edit_.set_revision((static_cast<std::uint64_t>(GetTickCount64()) << 16U) |
              static_cast<std::uint64_t>(GetCurrentProcessId()));
  EnableWindow(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), FALSE);
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
  (void)start_daemon();
  SetTimer(shell_.window, live_channel_timer, live_edit_.timer_interval(), nullptr);
  set_status(L"Ready — connecting to LayerD.");
}

std::optional<LRESULT> EditorWindow::handle_main_command(
    const WPARAM wparam) {
    const int id = LOWORD(wparam);
    const auto notification = HIWORD(wparam);
    if (const auto *binding = profile_field_binding(id);
        binding != nullptr) {
      if (!shell_.syncing && notification == binding->notification) {
        sync_profile_from_controls();
        if (binding->refresh_cooldown_label)
          refresh_selected_cooldown_label();
        record_profile_change(binding->change == ProfileFieldChange::text
                                  ? ChangeKind::text
                                  : ChangeKind::immediate);
        if (binding->invalidate_preview)
          InvalidateRect(panel_preview_.window, nullptr, TRUE);
        if (notification == BN_CLICKED)
          InvalidateRect(GetDlgItem(shell_.window, id), nullptr, TRUE);
      }
      return TRUE;
    }
    if (id == IDC_ADVANCED && notification == BN_CLICKED) {
      shell_.advanced_visible =
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_ADVANCED));
      InvalidateRect(GetDlgItem(shell_.window, IDC_ADVANCED), nullptr, TRUE);
      layout();
      return TRUE;
    }
    if (id == IDC_APPLICATION_EXACT_PATH &&
        notification == BN_CLICKED) {
      const bool exact = owner_draw_checkbox_checked(
          GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH));
      InvalidateRect(GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH), nullptr,
                     TRUE);
      if (!exact && !document_.profile().game.executable_path.empty()) {
        sync_profile_from_controls();
        if (document_.profile().game.executable_name.empty()) {
          document_.profile().game.executable_name = wide_to_utf8(
              std::filesystem::path(to_wide(document_.profile().game.executable_path))
                  .filename()
                  .wstring());
        }
        document_.profile().game.executable_path.clear();
        set_text(shell_.application_path,
                 document_.profile().game.executable_name.empty()
                     ? L"Choose a running application."
                     : to_wide(document_.profile().game.executable_name));
        record_profile_change(ChangeKind::immediate);
      } else if (exact && document_.profile().game.executable_path.empty()) {
        MessageBoxW(shell_.window,
                    L"Choose a running application to capture its exact "
                    L"executable path.",
                    L"Exact path matching", MB_OK | MB_ICONINFORMATION);
        show_tasks();
      }
      return TRUE;
    }
    if (id == IDC_DIRECT_DRAG && notification == BN_CLICKED) {
      const bool enabled =
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_DIRECT_DRAG));
      set_direct_drag_enabled(enabled);
      InvalidateRect(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), nullptr, TRUE);
      return TRUE;
    }
    if (id == IDC_VISUAL_ENABLED && notification == BN_CLICKED) {
      set_visual_assignment_enabled(
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_VISUAL_ENABLED)));
      return TRUE;
    }
    if (id == IDC_VISUAL_ISOLATED && notification == BN_CLICKED) {
      bool isolated =
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_VISUAL_ISOLATED));
      if (isolated &&
          MessageBoxW(shell_.window,
                      L"Isolated capture starts one bounded layerw.exe "
                      L"for this game window while visual recognition is "
                      L"active. It improves crash containment but uses "
                      L"additional process memory. Continue?",
                      L"Isolate visual capture",
                      MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        isolated = false;
        set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_VISUAL_ISOLATED),
                                false);
      }
      document_.profile().visual_runtime =
          isolated ? layerc::core::VisualRuntime::isolated
                   : layerc::core::VisualRuntime::inline_process;
      record_profile_change(ChangeKind::immediate);
      refresh_visual_controls();
      return TRUE;
    }
    switch (id) {
    case IDM_FILE_NEW:
      new_profile();
      return TRUE;
    case IDM_FILE_SAVE:
      (void)save_profile(false);
      return TRUE;
    case IDM_FILE_SAVE_AS:
      (void)save_profile(true);
      return TRUE;
    case IDC_POSITION_ON_WINDOW:
      edit_on_window();
      return TRUE;
    case IDC_CHOOSE_APPLICATION:
      show_tasks();
      return TRUE;
    case IDM_FILE_OPTIONS:
      open_options();
      return TRUE;
    case IDM_TOOLS_APPEARANCE:
      open_appearance();
      return TRUE;
    case IDM_FILE_EXPORT_PACK:
      export_profile_pack();
      return TRUE;
    case IDM_FILE_IMPORT_PACK:
      import_profile_pack();
      return TRUE;
    case IDM_FILE_RUNTIME_DIAGNOSTICS:
      open_runtime_diagnostics();
      return TRUE;
    case IDM_FILE_AUTOMATION:
      automation_editor_->open();
      return TRUE;
    case IDM_HELP_ABOUT:
      MessageBoxW(shell_.window,
                  L"LayerC Editor\n\nCreate and edit LayerC cooldown "
                  L"profiles using passive input only.",
                  L"About LayerC Editor", MB_OK | MB_ICONINFORMATION);
      return TRUE;
    case IDM_FILE_SPLIT_BARS:
      split_independent_bars();
      return TRUE;
    case IDC_ADD_COOLDOWN:
      add_cooldown();
      return TRUE;
    case IDC_REMOVE_COOLDOWN:
      remove_cooldown();
      return TRUE;
    case IDC_CAPTURE_BINDING:
      begin_binding_capture();
      return TRUE;
    case IDC_CAPTURE_HID:
      capture_hid_binding();
      return TRUE;
    case IDC_CALIBRATE_VISUAL:
      calibrate_visual();
      return TRUE;
    case IDC_REMOVE_VISUAL:
      remove_visual_assignment();
      return TRUE;
    case IDC_BAR_FILL:
    case IDC_BAR_BACKGROUND:
    case IDC_BAR_BORDER:
    case IDC_BAR_TEXT:
      choose_bar_color(id);
      return TRUE;
    case IDM_FILE_DELETE:
      delete_profile();
      return TRUE;
    case IDM_FILE_OPEN_FOLDER:
      open_profiles_folder();
      return TRUE;
    case IDM_FILE_EXIT:
      SendMessageW(shell_.window, WM_CLOSE, 0, 0);
      return TRUE;
    case IDM_EDIT_UNDO:
      undo();
      return TRUE;
    case IDM_EDIT_REDO:
      redo();
      return TRUE;
    default:
      break;
    }
    if (id == IDC_COOLDOWN_LIST && notification == LBN_SELCHANGE) {
      cancel_binding_capture();
      refresh_binding_controls();
      return TRUE;
    }
    if (id == IDC_BAR_LIST && notification == LBN_SELCHANGE) {
      sync_profile_from_controls();
      refresh_bar_controls();
      if (live_edit_.session_id() != 0) {
        if (const auto *panel = selected_bar(); panel != nullptr) {
          live_edit_.advance_revision();
          (void)live_edit_.channel().send({
              .type = layerc::core::LiveMessageType::select_panel,
              .revision = live_edit_.revision(),
              .edit_session = live_edit_.session_id(),
              .panel_id = panel->id,
          });
        }
      }
      return TRUE;
    }
  return std::nullopt;
}

std::optional<LRESULT> EditorWindow::handle_main_notify(
    const LPARAM lparam) {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_PROFILE_LIST) {
      if (header->code == NM_CLICK) {
        const auto *click = reinterpret_cast<const NMITEMACTIVATE *>(lparam);
        LVHITTESTINFO hit{};
        hit.pt = click->ptAction;
        const int row = ListView_HitTest(shell_.profile_list, &hit);
        if (row >= 0 && (hit.flags & LVHT_ONITEMSTATEICON) != 0 &&
            static_cast<std::size_t>(row) < shell_.profiles.size()) {
          const bool enabled = !shell_.profiles[static_cast<std::size_t>(row)].enabled;
          auto sync = shell_.sync_scope();
          ListView_SetCheckState(shell_.profile_list, row, enabled);
          save_profile_enabled(row, enabled);
          return TRUE;
        }
      } else if (header->code == LVN_ITEMCHANGED) {
        const auto *change = reinterpret_cast<const NMLISTVIEW *>(lparam);
        if (shell_.syncing)
          return TRUE;
        if ((change->uChanged & LVIF_STATE) != 0 &&
            ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) !=
                0) {
          save_profile_enabled(
              change->iItem,
              ListView_GetCheckState(shell_.profile_list, change->iItem) != FALSE);
          return TRUE;
        }
        if ((change->uChanged & LVIF_STATE) != 0 &&
            (change->uNewState & LVIS_SELECTED) != 0 &&
            (change->uOldState & LVIS_SELECTED) == 0) {
          open_profile_row(change->iItem, true);
          return TRUE;
        }
      }
    }
  return std::nullopt;
}

std::optional<LRESULT> EditorWindow::handle_main_draw_item(
    const LPARAM lparam) {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTVIEW && draw->CtlID == IDC_PROFILE_LIST) {
      draw_profile_item(*draw);
      return TRUE;
    }
    if (draw->CtlType == ODT_LISTBOX) {
      draw_list_box_item(*draw);
      return TRUE;
    }
    if (draw->CtlType != ODT_BUTTON)
      return std::nullopt;
    const int id = static_cast<int>(draw->CtlID);
    if (id == IDC_SHOW_TEXT || id == IDC_HIDE_READY ||
        id == IDC_DIRECT_DRAG || id == IDC_VISUAL_ENABLED ||
        id == IDC_VISUAL_ISOLATED || id == IDC_ADVANCED ||
        id == IDC_APPLICATION_EXACT_PATH) {
      draw_checkbox(*draw);
      return TRUE;
    }
    std::optional<COLORREF> overlay_color;
    if (const auto *panel = selected_bar()) {
      if (id == IDC_BAR_FILL)
        overlay_color = native_color_ref(panel->bar.fill_color);
      else if (id == IDC_BAR_BACKGROUND)
        overlay_color = native_color_ref(panel->bar.background_color);
      else if (id == IDC_BAR_BORDER)
        overlay_color = native_color_ref(panel->bar.border_color);
      else if (id == IDC_BAR_TEXT)
        overlay_color = native_color_ref(panel->bar.text_color);
    }
    draw_button(*draw, layerc::platform::EditorControlRole::button,
                overlay_color);
    return TRUE;
  }

LRESULT EditorWindow::handle_main_message(const HWND window, const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  switch (message) {
  case WM_SIZE:
    layout();
    return TRUE;
  case WM_DPICHANGED: {
    apply_suggested_dpi_rect(window, lparam);
    const UINT dpi = HIWORD(wparam);
    const HFONT old_font = shell_.font;
    const HFONT new_font = layerc::platform::create_native_dialog_font(dpi);
    if (new_font != nullptr) {
      shell_.font = new_font;
      style_dialog_children(window);
      if (old_font != nullptr)
        DeleteObject(old_font);
    }
    rebuild_profile_check_hit_images();
    layout();
    return TRUE;
  }
  case WM_TIMER:
    if (wparam == autosave_timer) {
      (void)flush_autosave(false);
      return TRUE;
    }
    if (wparam == live_channel_timer) {
      poll_live_channel();
      return TRUE;
    }
    break;
  case WM_GETMINMAXINFO: {
    const int dpi = window_base_dpi(window);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                        &monitor) != FALSE) {
      const int work_width = monitor.rcWork.right - monitor.rcWork.left;
      const int work_height = monitor.rcWork.bottom - monitor.rcWork.top;
      const int layout_dpi =
          geometry_dpi_for_extent(dpi, work_width, work_height, 1080, 700);
      reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {
          std::min(work_width, dpi_scaled(1080, layout_dpi)),
          std::min(work_height, dpi_scaled(700, layout_dpi))};
    } else {
      reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {
          dpi_scaled(1080, dpi), dpi_scaled(700, dpi)};
    }
    return TRUE;
  }
  case WM_COMMAND:
    if (const auto result = handle_main_command(wparam))
      return *result;
    break;
  case WM_INITMENUPOPUP: {
    const HMENU popup = reinterpret_cast<HMENU>(wparam);
    const bool has_grouped_bars =
        std::ranges::any_of(document_.profile().panels,
                            [](const layerc::core::Panel &panel) {
                              return panel.style ==
                                         layerc::core::PanelStyle::bar &&
                                     panel.cooldown_ids.size() > 1;
                            });
    const auto enable_item = [popup](const UINT item, const bool enabled) {
      EnableMenuItem(popup, item,
                     MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    enable_item(IDM_FILE_SPLIT_BARS, has_grouped_bars);
    enable_item(IDM_EDIT_UNDO, document_.history().can_undo());
    enable_item(IDM_EDIT_REDO, document_.history().can_redo());
    return TRUE;
  }
  case WM_NOTIFY:
    if (const auto result = handle_main_notify(lparam))
      return *result;
    break;
  case WM_DRAWITEM:
    if (const auto result = handle_main_draw_item(lparam))
      return *result;
    break;
  case WM_LBUTTONUP:
    if (task_picker_.picking()) {
      finish_picker();
      return TRUE;
    }
    break;
  case WM_KEYDOWN:
    if (task_picker_.picking() && wparam == VK_ESCAPE) {
      cancel_picker();
      set_status(L"Window picker cancelled.");
      return TRUE;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
      if (wparam == 'S') {
        (void)save_profile(false);
        return TRUE;
      }
      if (wparam == 'N') {
        new_profile();
        return TRUE;
      }
    }
    break;
  case WM_CAPTURECHANGED:
    if (task_picker_.picking())
      cancel_picker();
    break;
  case WM_CLOSE:
    if (confirm_unsaved_changes()) {
      end_edit_on_window();
      DestroyWindow(window);
    }
    return TRUE;
  case WM_DESTROY:
    cancel_picker();
    KillTimer(window, autosave_timer);
    KillTimer(window, live_channel_timer);
    if (task_picker_.window != nullptr && IsWindow(task_picker_.window))
      DestroyWindow(task_picker_.window);
    task_picker_.window = nullptr;
    if (shell_.profile_check_hit_images != nullptr) {
      ImageList_Destroy(shell_.profile_check_hit_images);
      shell_.profile_check_hit_images = nullptr;
    }
    for (auto &brush_value : appearance_editor_.brushes()) {
      if (brush_value != nullptr)
        DeleteObject(brush_value);
      brush_value = nullptr;
    }
    destroy_dialog_font(shell_.font);
    PostQuitMessage(0);
    return TRUE;
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

int run_editor(const HINSTANCE instance, const int show_command) {
  using layerc::platform::SingleInstanceGuard;
  const SingleInstanceGuard guard(editor_single_instance_name().c_str());
  if (guard.state() == SingleInstanceGuard::State::failed) {
    MessageBoxW(nullptr,
                L"LayerC Editor could not create its single-instance guard.",
                L"LayerC Editor", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (guard.state() == SingleInstanceGuard::State::already_running) {
    raise_existing_editor();
    return 0;
  }
  return std::make_unique<EditorWindow>()->run(instance, show_command);
}

} // namespace layerc::editor
