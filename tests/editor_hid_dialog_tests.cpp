#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

namespace layerc::test::editor_dialog {

void hid_dialog_contracts(const HINSTANCE instance, bool &passed) {
  const HWND hid_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_HID_BINDING), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      hid_dialog != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_DEVICE) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_CAPTURE) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_THRESHOLD) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_HYSTERESIS) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_SLOT) != nullptr,
      "the HID capture/refine resource should expose every V2.2 control");
  if (hid_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        hid_dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND advanced = GetDlgItem(hid_dialog, IDC_HID_SYSTEM_CLASS);
    passed &= expect(
        (GetWindowLongPtrW(advanced, GWL_STYLE) & BS_TYPEMASK) ==
            BS_AUTOCHECKBOX,
        "the system-class HID opt-in must retain explicit checkbox semantics");
    passed &= expect(LOWORD(SendMessageW(hid_dialog, DM_GETDEFID, 0, 0)) ==
                         IDC_HID_APPLY,
                     "Use Binding should remain the HID dialog default action");
    DestroyWindow(hid_dialog);
  }
}

} // namespace layerc::test::editor_dialog
