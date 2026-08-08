#include "editor_resource.h"
#include "editor_window.hpp"

#include <iostream>
#include <optional>
#include <string>

#include <commctrl.h>
#include <windows.h>

namespace {

int apply_commands{};

INT_PTR CALLBACK test_dialog_proc(const HWND window, const UINT message,
                                  const WPARAM wparam, LPARAM) {
  if (message == WM_COMMAND && LOWORD(wparam) == IDC_OPTIONS_APPLY) {
    ++apply_commands;
    return TRUE;
  }
  if (message == WM_CLOSE) {
    DestroyWindow(window);
    return TRUE;
  }
  return FALSE;
}

bool expect(const bool condition, const char *const message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_OPTIONS), nullptr, &test_dialog_proc, 0);
  if (!expect(dialog != nullptr, "the native Options dialog should load"))
    return 1;

  layerc::editor::style_native_dialog_children(
      dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
  const HWND apply = GetDlgItem(dialog, IDC_OPTIONS_APPLY);
  const HWND checkbox = GetDlgItem(dialog, IDC_CAPTURE_PRIVACY);
  const auto apply_type =
      GetWindowLongPtrW(apply, GWL_STYLE) & BS_TYPEMASK;
  const auto checkbox_type =
      GetWindowLongPtrW(checkbox, GWL_STYLE) & BS_TYPEMASK;
  bool passed = true;
  passed &= expect(apply_type == BS_DEFPUSHBUTTON,
                   "the default Apply button must retain dialog-manager semantics");
  passed &= expect(checkbox_type == BS_AUTOCHECKBOX,
                   "the capture checkbox must retain native auto-toggle semantics");

  SendMessageW(checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(checkbox, BM_CLICK, 0, 0);
  passed &= expect(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED,
                   "a themed native checkbox should toggle through BM_CLICK");

  const LRESULT default_id = SendMessageW(dialog, DM_GETDEFID, 0, 0);
  passed &= expect(HIWORD(default_id) == DC_HASDEFID &&
                       LOWORD(default_id) == IDC_OPTIONS_APPLY,
                   "the Options dialog should advertise Apply as its default action");
  MSG enter{.hwnd = checkbox,
            .message = WM_KEYDOWN,
            .wParam = VK_RETURN,
            .lParam = 1};
  (void)IsDialogMessageW(dialog, &enter);
  passed &= expect(apply_commands == 1,
                   "Enter should dispatch the modal dialog's default action");

  layerc::core::GameProfile original;
  original.id = "profile";
  original.name = "Original";
  layerc::editor::EditorProfileHistory history;
  history.reset(original);
  passed &= expect(!history.is_dirty(original, true),
                   "a persisted profile matching its saved snapshot should be clean");
  passed &= expect(history.is_dirty(original, false),
                   "a new draft must remain dirty at its initial history snapshot");
  auto edited = original;
  edited.name = "First text";
  passed &= expect(history.record(edited, true),
                   "the first text edit should create a history transaction");
  edited.name = "Coalesced text";
  passed &= expect(history.record(edited, true),
                   "a later text edit should update the coalesced transaction");
  auto undone = history.undo(edited);
  passed &= expect(undone.has_value() && undone->name == "Original" &&
                       history.can_redo(),
                   "coalesced text edits should undo as one transaction");
  auto redone = undone.has_value() ? history.redo(*undone) : std::nullopt;
  passed &= expect(redone.has_value() && redone->name == "Coalesced text",
                   "a coalesced editor transaction should redo exactly");
  if (undone.has_value() && redone.has_value()) {
    passed &= expect(history.is_dirty(*undone, false) &&
                         history.is_dirty(*redone, false),
                     "draft undo and redo states must remain dirty until first save");
  }

  if (redone.has_value()) {
    auto immediate = *redone;
    immediate.enabled = false;
    history.end_text_coalescing();
    passed &= expect(history.record(immediate, false),
                     "an immediate edit should create a separate transaction");
    history.rebase(immediate);
    passed &= expect(history.is_saved(immediate),
                     "rebase should mark the successfully persisted profile");
    const auto undo_immediate = history.undo(immediate);
    passed &= expect(undo_immediate.has_value() && undo_immediate->enabled,
                     "autosave rebase should preserve earlier undo history");
    if (undo_immediate.has_value()) {
      passed &= expect(!history.is_saved(*undo_immediate),
                       "undo away from the persisted state should remain dirty");
      const auto redo_immediate = history.redo(*undo_immediate);
      passed &= expect(redo_immediate.has_value() &&
                           history.is_saved(*redo_immediate),
                       "redo back to the persisted state should clear dirty state");
    }
  }

  history.reset(original);
  auto bounded = original;
  for (int index = 0; index < 60; ++index) {
    bounded.name = "Edit " + std::to_string(index);
    (void)history.record(bounded, false);
  }
  int undo_count{};
  while (const auto previous = history.undo(bounded)) {
    bounded = *previous;
    ++undo_count;
  }
  passed &= expect(undo_count == 50,
                   "editor undo history should retain exactly 50 transactions");

  const auto normal_checkbox = layerc::editor::profile_checkbox_metrics(96);
  const auto high_dpi_checkbox = layerc::editor::profile_checkbox_metrics(192);
  passed &= expect(normal_checkbox.image_size == 16 &&
                       normal_checkbox.inset == 2 &&
                       normal_checkbox.pen_width == 2,
                   "profile checkbox resources should retain their 96-DPI metrics");
  passed &= expect(high_dpi_checkbox.image_size == 32 &&
                       high_dpi_checkbox.inset == 4 &&
                       high_dpi_checkbox.pen_width == 4,
                   "profile checkbox resources and hit target should scale with DPI");

  DestroyWindow(dialog);
  return passed ? 0 : 1;
}
