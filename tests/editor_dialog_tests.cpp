#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

#include <string_view>

#include <commctrl.h>
#include <windows.h>

namespace {

int apply_commands{};

using layerc::test::expect;

} // namespace

namespace layerc::test::editor_dialog {

INT_PTR CALLBACK test_dialog_proc(const HWND window, const UINT message,
                                  const WPARAM wparam, LPARAM) {
  if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
    ++apply_commands;
    return TRUE;
  }
  if (message == WM_CLOSE) {
    DestroyWindow(window);
    return TRUE;
  }
  return FALSE;
}

} // namespace layerc::test::editor_dialog

void editor_dialog_contracts() {
  using namespace layerc::test::editor_dialog;

  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_OPTIONS), nullptr, &test_dialog_proc, 0);
  if (dialog == nullptr) {
    (void)expect(false, "the native Options dialog should load");
    return;
  }

  layerc::editor::style_native_dialog_children(
      dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
  const HWND apply = GetDlgItem(dialog, IDOK);
  const HWND checkbox = GetDlgItem(dialog, IDC_CAPTURE_PRIVACY);
  const HWND visual_checkbox = GetDlgItem(dialog, IDC_VISUAL_MASTER);
  const auto apply_type = GetWindowLongPtrW(apply, GWL_STYLE) & BS_TYPEMASK;
  const auto checkbox_type =
      GetWindowLongPtrW(checkbox, GWL_STYLE) & BS_TYPEMASK;
  const auto visual_checkbox_type =
      GetWindowLongPtrW(visual_checkbox, GWL_STYLE) & BS_TYPEMASK;
  bool passed = true;
  passed &=
      expect(apply_type == BS_OWNERDRAW &&
                 LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDOK &&
                 GetDlgItem(dialog, IDC_OPTIONS_APPLY) != nullptr &&
                 GetDlgItem(dialog, IDC_EDITOR_CAPTURE_PRIVACY) != nullptr,
             "the themed OK button must retain dialog-manager default semantics");
  passed &=
      expect(checkbox_type == BS_AUTOCHECKBOX,
             "the capture checkbox must retain native auto-toggle semantics");
  passed &= expect(
      visual_checkbox_type == BS_AUTOCHECKBOX,
      "the visual master checkbox must retain native auto-toggle semantics");
  const HWND updates_checkbox = GetDlgItem(dialog, IDC_LAUNCHER_UPDATES);
  passed &= expect(
      updates_checkbox != nullptr &&
          (GetWindowLongPtrW(updates_checkbox, GWL_STYLE) & BS_TYPEMASK) ==
              BS_AUTOCHECKBOX &&
          GetDlgItem(dialog, IDC_OPTIONS_UPDATES_HINT) != nullptr,
      "Options should offer the launcher update check as a native checkbox "
      "with its hint");

  SendMessageW(checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(checkbox, BM_CLICK, 0, 0);
  passed &= expect(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED,
                   "a themed native checkbox should toggle through BM_CLICK");
  SendMessageW(visual_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(visual_checkbox, BM_CLICK, 0, 0);
  passed &= expect(
      SendMessageW(visual_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED,
      "the themed visual master checkbox should toggle through BM_CLICK");

  const LRESULT default_id = SendMessageW(dialog, DM_GETDEFID, 0, 0);
  passed &=
      expect(HIWORD(default_id) == DC_HASDEFID &&
                 LOWORD(default_id) == IDOK,
             "the Options dialog should advertise OK as its default action");
  MSG enter{};
  enter.hwnd = checkbox;
  enter.message = WM_KEYDOWN;
  enter.wParam = VK_RETURN;
  enter.lParam = 1;
  (void)IsDialogMessageW(dialog, &enter);
  passed &= expect(apply_commands == 1,
                   "Enter should dispatch the modal dialog's default action");

  profile_history_contracts(passed);
  profile_checkbox_contracts(passed);
  source_schema_contracts(passed);
  appearance_draft_contracts(passed);
  automation_preset_contracts(passed);
  cooldown_edit_contracts(passed);
  main_dialog_contracts(instance, passed);
  appearance_dialog_contracts(instance, passed);
  tasks_dialog_contracts(instance, passed);
  pack_dialog_contracts(instance, passed);
  automation_dialog_contracts(instance, passed);
  hid_dialog_contracts(instance, passed);
  diagnostics_dialog_contracts(instance, passed);
  task_picker_contracts(passed);
  panel_preview_contracts(passed);
  binding_capture_contracts(passed);

  DestroyWindow(dialog);
}

void automation_palette_contracts();

void editor_draft_leave_contracts();
void editor_dialog_font_contracts();
void editor_calibration_shutdown_contracts();
void editor_calibration_disabled_owner_contracts();
void editor_calibration_destroyed_owner_contracts();
void editor_asset_history_contracts();
void editor_asset_pending_publication_contracts();
void editor_binding_row_contracts();
void editor_cooldown_component_contracts();
void editor_visual_component_contracts();
void editor_drag_transaction_contracts();
void editor_automation_component_contracts();
void editor_theme_contrast_contracts();
void editor_theme_high_contrast_contracts();
void editor_status_glyph_contracts();
void editor_dialog_layout_contracts();
void editor_shortcut_contracts();
void editor_main_layout_contracts();
void editor_accessibility_contracts();

int main(const int argc, char **argv) {
  if (argc == 5 && std::string_view(argv[1]) == "--request-pipe" &&
      std::string_view(argv[3]) == "--response-pipe") {
    // Disposable helper mode: never reads its inherited pipe, captures input,
    // opens settings, or runs editor tests. Its owning Job must stop it.
    Sleep(INFINITE);
    return ERROR_CANCELLED;
  }
  return layerc::test::run(
      {{"editor_dialog_contracts", editor_dialog_contracts},
       {"automation_palette_contracts", automation_palette_contracts},
       {"editor_draft_leave_contracts", editor_draft_leave_contracts},
       {"editor_dialog_font_contracts", editor_dialog_font_contracts},
       {"editor_calibration_shutdown_contracts",
        editor_calibration_shutdown_contracts},
       {"editor_calibration_disabled_owner_contracts",
        editor_calibration_disabled_owner_contracts},
       {"editor_calibration_destroyed_owner_contracts",
        editor_calibration_destroyed_owner_contracts},
       {"editor_asset_history_contracts", editor_asset_history_contracts},
       {"editor_asset_pending_publication_contracts",
        editor_asset_pending_publication_contracts},
       {"editor_binding_row_contracts", editor_binding_row_contracts},
       {"editor_cooldown_component_contracts",
        editor_cooldown_component_contracts},
       {"editor_automation_component_contracts",
        editor_automation_component_contracts},
       {"editor_visual_component_contracts",
        editor_visual_component_contracts},
       {"editor_drag_transaction_contracts",
        editor_drag_transaction_contracts},
       {"editor_theme_contrast_contracts", editor_theme_contrast_contracts},
       {"editor_theme_high_contrast_contracts",
        editor_theme_high_contrast_contracts},
       {"editor_status_glyph_contracts", editor_status_glyph_contracts},
       {"editor_dialog_layout_contracts", editor_dialog_layout_contracts},
       {"editor_shortcut_contracts", editor_shortcut_contracts},
       {"editor_main_layout_contracts", editor_main_layout_contracts},
       {"editor_accessibility_contracts", editor_accessibility_contracts}});
}
