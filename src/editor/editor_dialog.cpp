#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_app.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_layout_rules.hpp"
#include "editor_window.hpp"
#include "panel_items.hpp"
#include "static_style.hpp"

#include "editor_resource.h"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/editor_appearance.hpp"
#include "layerc/platform/editor_instance.hpp"
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
#include <objbase.h>
#include <windows.h>
#include <windowsx.h>
namespace layerc::editor {

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
    themed_message_box(nullptr,
                (L"LayerC could not read or migrate editor appearance settings. " +
                 detail)
                    .c_str(),
                L"LayerC settings", MB_OK | MB_ICONWARNING);
  }
  layerc::platform::set_native_text_scale(stored_text_scale());
  const auto capture = layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::editor_capture_exclusion);
  if (capture.value)
    shell_.capture_exclusion = *capture.value;
  if (capture.failed()) {
    const auto detail = layerc::platform::windows_error_text(capture.error);
    themed_message_box(nullptr,
                (L"LayerC could not read the capture-exclusion setting. " +
                 detail)
                    .c_str(),
                L"LayerC settings", MB_OK | MB_ICONWARNING);
  }
  const auto recovered = layerc::platform::recover_profile_transactions(
      layerc::platform::profile_directory());
  if (const auto error = recovered.failure; error.has_value()) {
    const auto message =
        L"LayerC Editor could not recover an interrupted profile import (" +
        layerc::platform::utf8_to_wide(
            layerc::platform::profile_write_error_text(*error)) +
        L"). Resolve the _transactions directory before editing profiles.";
    themed_message_box(nullptr, message.c_str(), L"LayerC profile recovery",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  // The Browse dialog (IFileOpenDialog) needs an STA on the UI thread.
  const HRESULT com = CoInitializeEx(
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  struct ComScope {
    bool initialized;
    ~ComScope() {
      if (initialized)
        CoUninitialize();
    }
  } com_scope{SUCCEEDED(com)};
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
  if (!restore_window_placement(show_command)) {
    SetWindowPos(shell_.window, nullptr, left, top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(shell_.window, show_command);
  }
  UpdateWindow(shell_.window);
  const HACCEL accelerators =
      LoadAcceleratorsW(shell_.instance, MAKEINTRESOURCEW(IDR_EDITOR_ACCELERATORS));
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (cooldown_editor_.handle_capture_message(message))
      continue;
    if (handle_list_key(message))
      continue;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
      if (task_picker_.picking()) {
        cancel_picker();
        set_status(L"Window picker cancelled.");
        continue;
      }
      if ((message.hwnd == shell_.window || IsChild(shell_.window, message.hwnd)) &&
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_DIRECT_DRAG))) {
        set_direct_drag_enabled(false);
        continue;
      }
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
  layerc::platform::set_window_app_icon(window);
  const UINT dpi = GetDpiForWindow(window);
  shell_.font = layerc::platform::create_native_dialog_font(dpi);
  shell_.heading_font = layerc::platform::create_native_font(
      layerc::platform::NativeFontKind::heading, dpi);
  shell_.title_font = layerc::platform::create_native_font(
      layerc::platform::NativeFontKind::title, dpi);
  appearance_editor_.set_appearance(appearance_editor_.draft().staged().theme,
                                    appearance_editor_.draft().staged().accent);
  publish_chrome_palette();
  apply_dialog_chrome(shell_.window, shell_.capture_exclusion);
  layerc::platform::install_dark_menu_bar(shell_.window);
  style_dialog_children(shell_.window);
  rebuild_brushes();
  shell_.profile_list = GetDlgItem(shell_.window, IDC_PROFILE_LIST);
  cooldown_editor_.bind(GetDlgItem(shell_.window, IDC_COOLDOWN_LIST));
  panel_preview_.list = GetDlgItem(shell_.window, IDC_BAR_LIST);
  panel_preview_.window = GetDlgItem(shell_.window, IDC_PREVIEW);
  shell_.status = GetDlgItem(shell_.window, IDC_STATUS);
  shell_.application_path = GetDlgItem(shell_.window, IDC_APPLICATION_PATH);
  apply_heading_fonts();
  shell_.advanced_visible =
      layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::editor_advanced_view)
          .value.value_or(false);
  set_text(GetDlgItem(shell_.window, IDC_ADVANCED),
           shell_.advanced_visible ? L"Hide advanced" : L"Show advanced");
  install_shell_helpers();
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
  SetWindowSubclass(shell_.profile_list, &EditorWindow::profile_list_subclass, 1,
                    reinterpret_cast<DWORD_PTR>(this));
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
  live_edit_.seed_revision((static_cast<std::uint64_t>(GetTickCount64()) << 16U) |
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
        (void)apply_profile_controls(binding->change == ProfileFieldChange::text
                                         ? ChangeKind::text
                                         : ChangeKind::immediate);
        if (binding->refresh_cooldown_label)
          cooldown_editor_.refresh_selected_label();
        if (binding->invalidate_preview)
          InvalidateRect(panel_preview_.window, nullptr, TRUE);
        if (notification == BN_CLICKED)
          InvalidateRect(GetDlgItem(shell_.window, id), nullptr, TRUE);
      }
      return TRUE;
    }
    if (handle_item_command(id, notification)) return TRUE;
    if ((id == IDC_ADVANCED && notification == BN_CLICKED) ||
        id == IDM_VIEW_ADVANCED) {
      set_advanced_visible(!shell_.advanced_visible, true);
      return TRUE;
    }
    if (id == IDC_APPLICATION_PATH && notification == EN_CHANGE) {
      apply_application_text();
      return TRUE;
    }
    if (id == IDC_APPLICATION_EXACT_PATH &&
        notification == BN_CLICKED) {
      const bool exact = owner_draw_checkbox_checked(
          GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH));
      InvalidateRect(GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH), nullptr,
                     TRUE);
      if (!exact && !document_.profile().game.executable_path.empty()) {
        (void)apply_profile_change(
            ChangeKind::immediate, [](layerc::core::GameProfile &profile) {
              if (profile.game.executable_name.empty())
                profile.game.executable_name = wide_to_utf8(
                    std::filesystem::path(to_wide(profile.game.executable_path))
                        .filename()
                        .wstring());
              profile.game.executable_path.clear();
            });
        auto sync = shell_.sync_scope();
        set_text(shell_.application_path,
                 to_wide(document_.profile().game.executable_name));
        describe_application();
      } else if (exact && document_.profile().game.executable_path.empty()) {
        const std::filesystem::path typed(window_text(shell_.application_path));
        if (typed.is_absolute()) {
          apply_application_text();
        } else {
          set_owner_draw_checkbox(
              GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH), false);
          set_status(L"Browse to the .exe or choose a running application to "
                     L"match its exact path.");
        }
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
      visual_editor_.set_enabled(
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_VISUAL_ENABLED)));
      return TRUE;
    }
    if (id == IDC_VISUAL_ISOLATED && notification == BN_CLICKED) {
      bool isolated =
          owner_draw_checkbox_checked(GetDlgItem(shell_.window, IDC_VISUAL_ISOLATED));
      if (isolated &&
          themed_message_box(shell_.window,
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
      (void)apply_profile_change(
          ChangeKind::immediate, [&](layerc::core::GameProfile &profile) {
            profile.visual_runtime =
                isolated ? layerc::core::VisualRuntime::isolated
                         : layerc::core::VisualRuntime::inline_process;
          });
      visual_editor_.refresh();
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
    case IDC_BROWSE_APPLICATION:
      browse_application();
      return TRUE;
    case IDC_PROFILE_NEW:
      new_profile();
      return TRUE;
    case IDC_PROFILE_DELETE:
      delete_profile();
      return TRUE;
    case IDC_PROFILE_MORE: {
      RECT bounds{};
      GetWindowRect(GetDlgItem(shell_.window, IDC_PROFILE_MORE), &bounds);
      const HMENU menus =
          LoadMenuW(shell_.instance, MAKEINTRESOURCEW(IDR_CONTEXT_MENUS));
      if (menus != nullptr) {
        const HMENU menu = GetSubMenu(menus, 0);
        const bool draft = document_.path().empty();
        ModifyMenuW(menu, IDM_PROFILE_TOGGLE, MF_BYCOMMAND | MF_STRING,
                    IDM_PROFILE_TOGGLE,
                    document_.profile().enabled ? L"&Turn off" : L"&Turn on");
        for (const UINT item : {IDM_PROFILE_TOGGLE, IDM_FILE_EXPORT_PACK})
          EnableMenuItem(menu, item,
                         MF_BYCOMMAND | (draft ? MF_GRAYED : MF_ENABLED));
        const UINT command = static_cast<UINT>(TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, bounds.left,
            bounds.top, shell_.window, nullptr));
        DestroyMenu(menus);
        if (command != 0)
          SendMessageW(shell_.window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
      }
      return TRUE;
    }
    case IDM_PROFILE_RENAME:
      focus_field(IDC_PROFILE_NAME);
      return TRUE;
    case IDM_PROFILE_DUPLICATE:
      duplicate_profile();
      return TRUE;
    case IDM_PROFILE_TOGGLE: {
      const int row = ListView_GetNextItem(shell_.profile_list, -1, LVNI_SELECTED);
      if (row >= 0 && static_cast<std::size_t>(row) < shell_.profiles.size() &&
          !shell_.profiles[static_cast<std::size_t>(row)].path.empty())
        ListView_SetCheckState(shell_.profile_list, row,
                               ListView_GetCheckState(shell_.profile_list, row) ==
                                   FALSE);
      return TRUE;
    }
    case IDM_COOLDOWN_ADD:
      cooldown_editor_.add();
      return TRUE;
    case IDM_COOLDOWN_DUPLICATE:
      cooldown_editor_.duplicate();
      return TRUE;
    case IDM_COOLDOWN_RENAME:
      focus_field(IDC_COOLDOWN_NAME);
      return TRUE;
    case IDM_COOLDOWN_REMOVE:
      cooldown_editor_.remove();
      return TRUE;
    case IDM_VIEW_TEXT_100:
    case IDM_VIEW_TEXT_115:
    case IDM_VIEW_TEXT_130:
      set_text_scale(id == IDM_VIEW_TEXT_130   ? 130
                     : id == IDM_VIEW_TEXT_115 ? 115
                                               : 100);
      return TRUE;
    case IDM_HELP_SHORTCUTS:
      show_keyboard_shortcuts();
      return TRUE;
    case IDM_HELP_ABOUT:
      show_about();
      return TRUE;
    case IDM_FILE_SPLIT_BARS:
      split_independent_bars();
      return TRUE;
    case IDC_ADD_COOLDOWN:
      cooldown_editor_.add();
      return TRUE;
    case IDC_REMOVE_COOLDOWN:
      cooldown_editor_.remove();
      return TRUE;
    case IDC_CAPTURE_BINDING:
      cooldown_editor_.begin_capture();
      return TRUE;
    case IDC_CAPTURE_HID:
      cooldown_editor_.capture_hid();
      return TRUE;
    case IDC_CALIBRATE_VISUAL:
      visual_editor_.calibrate();
      return TRUE;
    case IDC_REMOVE_VISUAL:
      visual_editor_.remove_assignment();
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
      cooldown_editor_.cancel_capture();
      cooldown_editor_.refresh_controls();
      return TRUE;
    }
    if (id == IDC_BAR_LIST && notification == LBN_SELCHANGE) {
      (void)apply_profile_controls(DocumentChangeKind::text);
      refresh_bar_controls();
      // Keep the cooldown selection on a cooldown this bar shows.
      if (const auto *panel = selected_bar(); panel != nullptr) {
        const auto members = panel_cooldown_members(*panel);
        const auto *cooldown = cooldown_editor_.selected();
        if (!members.empty() &&
            (cooldown == nullptr || !panel_has_cooldown(*panel, cooldown->id))) {
          const auto &cooldowns = document_.profile().cooldowns;
          const auto first = std::ranges::find(
              cooldowns, members.front(),
              &layerc::core::CooldownDefinition::id);
          if (first != cooldowns.end())
            cooldown_editor_.refresh_list(static_cast<int>(first - cooldowns.begin()));
        }
      }
      if (live_edit_.editing()) {
        if (const auto *panel = selected_bar(); panel != nullptr)
          (void)live_edit_.send_in_session(
              {.type = layerc::core::LiveMessageType::select_panel,
               .panel_id = panel->id});
      }
      return TRUE;
    }
  return std::nullopt;
}

void EditorWindow::reload_fonts() {
  if (shell_.window == nullptr)
    return;
  const UINT dpi = GetDpiForWindow(shell_.window);
  const auto replace = [dpi](HFONT &font,
                             const layerc::platform::NativeFontKind kind) {
    if (const HFONT created = layerc::platform::create_native_font(kind, dpi);
        created != nullptr) {
      if (font != nullptr)
        DeleteObject(font);
      font = created;
    }
  };
  replace(shell_.font, layerc::platform::NativeFontKind::body);
  replace(shell_.heading_font, layerc::platform::NativeFontKind::heading);
  replace(shell_.title_font, layerc::platform::NativeFontKind::title);
  style_dialog_children(shell_.window);
  apply_heading_fonts();
  rebuild_profile_check_hit_images();
  // Layout enforces the same measured minimum after font changes as resizing.
  layout();
}

LRESULT CALLBACK EditorWindow::profile_list_subclass(
    const HWND list, const UINT message, const WPARAM wparam,
    const LPARAM lparam, const UINT_PTR subclass_id, const DWORD_PTR data) {
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(list, &EditorWindow::profile_list_subclass,
                         subclass_id);
  } else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
    auto &self = *reinterpret_cast<EditorWindow *>(data);
    LVHITTESTINFO hit{};
    hit.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const int row = ListView_HitTest(list, &hit);
    RECT bounds{};
    if (row >= 0 && static_cast<std::size_t>(row) < self.shell_.profiles.size() &&
        !self.shell_.profiles[static_cast<std::size_t>(row)].path.empty() &&
        ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS) != FALSE &&
        profile_checkbox_hit(bounds, hit.pt, GetDpiForWindow(list))) {
      SetFocus(list);
      // Not under sync_scope: LVN_ITEMCHANGED commits the new state.
      ListView_SetCheckState(
          list, row, ListView_GetCheckState(list, row) == FALSE);
      return 0;
    }
  }
  return DefSubclassProc(list, message, wparam, lparam);
}

std::optional<LRESULT> EditorWindow::handle_main_notify(
    const LPARAM lparam) {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_PROFILE_LIST) {
      // Mouse toggles come from profile_list_subclass and Space from the
      // list view itself; both surface here exactly once.
      if (header->code == LVN_ITEMCHANGED) {
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
    if (draw->CtlType == ODT_STATIC && static_style(draw->hwndItem)) {
      draw_status_line(*draw);
      return TRUE;
    }
    if (draw->CtlType != ODT_BUTTON)
      return std::nullopt;
    const int id = static_cast<int>(draw->CtlID);
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
  case WM_DPICHANGED:
    apply_suggested_dpi_rect(window, lparam);
    reload_fonts();
    return TRUE;
  case WM_SYSCOLORCHANGE:
    // A contrast theme turned on or off, or its colors changed.
    refresh_theme();
    break;
  case WM_SETTINGCHANGE:
    if (wparam == SPI_SETHIGHCONTRAST)
      refresh_theme();
    break;
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
    // The layout reflows rather than shrinking text, so the minimum is what
    // the content needs: ~900 px wide (half of a 1920 px screen still
    // snaps) and the height of the last layout's rows.
    const int dpi = window_base_dpi(window);
    RECT frame{0, 0,
               std::max(shell_.min_client_width,
                        dpi_scaled(main_client_min_width, dpi)),
               std::max(shell_.min_client_height, dpi_scaled(520, dpi))};
    AdjustWindowRectExForDpi(
        &frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), TRUE,
        static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
        GetDpiForWindow(window));
    LONG width = frame.right - frame.left;
    LONG height = frame.bottom - frame.top;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                        &monitor) != FALSE) {
      width = std::min(width, monitor.rcWork.right - monitor.rcWork.left);
      height = std::min(height, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {width, height};
    return TRUE;
  }
  case WM_CONTEXTMENU:
    show_context_menu(reinterpret_cast<HWND>(wparam),
                      {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
    return TRUE;
  case WM_ERASEBKGND:
    erase_dialog_background(window, wparam);
    paint_status_strip(reinterpret_cast<HDC>(wparam));
    return TRUE;
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
    enable_item(IDM_EDIT_UNDO, document_.can_undo());
    enable_item(IDM_EDIT_REDO, document_.can_redo());
    const bool has_cooldown = cooldown_editor_.selected() != nullptr;
    enable_item(IDM_COOLDOWN_DUPLICATE, has_cooldown);
    enable_item(IDM_COOLDOWN_RENAME, has_cooldown);
    enable_item(IDM_COOLDOWN_REMOVE,
                has_cooldown && document_.profile().cooldowns.size() > 1);
    enable_item(IDM_FILE_EXPORT_PACK, !document_.path().empty());
    update_view_menu(popup);
    return TRUE;
  }
  case WM_NOTIFY:
    if (const auto result = handle_main_notify(lparam))
      return *result;
    if (const auto result =
            handle_shell_notify(*reinterpret_cast<const NMHDR *>(lparam))) {
      SetWindowLongPtrW(window, DWLP_MSGRESULT, *result);
      return TRUE;
    }
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
      save_window_placement();
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
    destroy_dialog_font(shell_.heading_font);
    destroy_dialog_font(shell_.title_font);
    PostQuitMessage(0);
    return TRUE;
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

int run_editor(const HINSTANCE instance, const int show_command) {
  using layerc::platform::SingleInstanceGuard;
  const SingleInstanceGuard guard(
      layerc::platform::editor_single_instance_name().c_str());
  if (guard.state() == SingleInstanceGuard::State::failed) {
    themed_message_box(nullptr,
                L"LayerC Editor could not create its single-instance guard.",
                L"LayerC Editor", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (guard.state() == SingleInstanceGuard::State::already_running) {
    layerc::platform::raise_existing_editor();
    return 0;
  }
  return std::make_unique<EditorWindow>()->run(instance, show_command);
}

} // namespace layerc::editor
