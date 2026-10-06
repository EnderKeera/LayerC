#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

namespace {

RECT child_rect(const HWND dialog, const int id) {
  RECT result{};
  const HWND child = GetDlgItem(dialog, id);
  if (child != nullptr && GetWindowRect(child, &result) != FALSE)
    MapWindowPoints(HWND_DESKTOP, dialog,
                    reinterpret_cast<POINT *>(&result), 2);
  return result;
}

} // namespace

namespace layerc::test::editor_dialog {

void pack_dialog_contracts(const HINSTANCE instance, bool &passed) {
  for (const int resource : {IDD_EXPORT_PACK, IDD_IMPORT_PACK}) {
    const HWND pack_dialog =
        CreateDialogParamW(instance, MAKEINTRESOURCEW(resource), nullptr,
                           &test_dialog_proc, 0);
    passed &= expect(
        pack_dialog != nullptr &&
            (GetWindowLongPtrW(pack_dialog, GWL_STYLE) & WS_THICKFRAME) != 0 &&
            GetDlgItem(pack_dialog, IDC_PACK_INTRO) != nullptr &&
            GetDlgItem(pack_dialog, IDC_PACK_LIST) != nullptr &&
            GetDlgItem(pack_dialog, IDC_PACK_CONFIRM) != nullptr,
        "pack dialogs should expose responsive list and action resources");
    if (pack_dialog != nullptr) {
      RECT client{};
      GetClientRect(pack_dialog, &client);
      const RECT list = child_rect(pack_dialog, IDC_PACK_LIST);
      const RECT confirm = child_rect(pack_dialog, IDC_PACK_CONFIRM);
      passed &= expect(
          (list.right - list.left) * 5 >= (client.right - client.left) * 4 &&
              list.bottom < confirm.top,
          "pack list should consume available width without covering actions");
      DestroyWindow(pack_dialog);
    }
  }
}

} // namespace layerc::test::editor_dialog
