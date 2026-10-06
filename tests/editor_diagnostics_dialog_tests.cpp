#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

namespace layerc::test::editor_dialog {

void diagnostics_dialog_contracts(const HINSTANCE instance, bool &passed) {
  const HWND diagnostics_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_RUNTIME_DIAGNOSTICS),
                         nullptr, &test_dialog_proc, 0);
  passed &= expect(
      diagnostics_dialog != nullptr &&
          (GetWindowLongPtrW(diagnostics_dialog, GWL_STYLE) & WS_THICKFRAME) !=
              0 &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_SUMMARY) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RAW_TOGGLE) !=
              nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_TEXT) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_REFRESH) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_COPY) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RETRY_VISUAL) !=
              nullptr,
      "runtime diagnostics should expose resizable summary, raw JSON, copy, "
      "and contextual retry controls");
  if (diagnostics_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        diagnostics_dialog,
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    passed &= expect(
        (GetWindowLongPtrW(
             GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RAW_TOGGLE),
             GWL_STYLE) &
         BS_TYPEMASK) == BS_AUTOCHECKBOX,
        "the raw diagnostics disclosure should retain native checkbox "
        "behavior");
    passed &= expect(
        LOWORD(SendMessageW(diagnostics_dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "Close should remain the Runtime Diagnostics default action");
    DestroyWindow(diagnostics_dialog);
  }
}

} // namespace layerc::test::editor_dialog
