#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "editor_state.hpp"
#include "test_support.hpp"

namespace layerc::test::editor_dialog {

void tasks_dialog_contracts(const HINSTANCE instance, bool &passed) {
  const HWND tasks_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_TASKS), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      tasks_dialog != nullptr &&
          (GetWindowLongPtrW(tasks_dialog, GWL_STYLE) & WS_THICKFRAME) != 0 &&
          GetDlgItem(tasks_dialog, IDC_TASK_LIST) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_PICK_WINDOW) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_USE_TASK) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_REFRESH_TASKS) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_TASK_ACTIONS) != nullptr,
      "Detected Tasks should remain resizable with all primary actions "
      "visible");
  if (tasks_dialog != nullptr)
    DestroyWindow(tasks_dialog);
}

void task_picker_contracts(bool &passed) {
  layerc::editor::TaskPicker task_picker_state;
  passed &= expect(!task_picker_state.has_request(),
                   "task picker should start without a target request");
  task_picker_state.request_live_edit();
  passed &= expect(task_picker_state.awaiting_live_edit() &&
                       !task_picker_state.awaiting_calibration(),
                   "live-edit target selection should be exclusive");
  task_picker_state.request_calibration();
  passed &= expect(task_picker_state.awaiting_calibration() &&
                       !task_picker_state.awaiting_live_edit(),
                   "calibration should replace any live-edit target request");
  task_picker_state.clear_request();
  passed &= expect(!task_picker_state.has_request(),
                   "clearing the task picker should remove its target request");
}

} // namespace layerc::test::editor_dialog
