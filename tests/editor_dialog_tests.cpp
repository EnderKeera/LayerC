#include "editor_resource.h"
#include "editor_window.hpp"
#include "../src/calibration/calibration_resource.h"

#include <algorithm>
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
  const HWND visual_checkbox = GetDlgItem(dialog, IDC_VISUAL_MASTER);
  const auto apply_type =
      GetWindowLongPtrW(apply, GWL_STYLE) & BS_TYPEMASK;
  const auto checkbox_type =
      GetWindowLongPtrW(checkbox, GWL_STYLE) & BS_TYPEMASK;
  const auto visual_checkbox_type =
      GetWindowLongPtrW(visual_checkbox, GWL_STYLE) & BS_TYPEMASK;
  bool passed = true;
  passed &= expect(apply_type == BS_DEFPUSHBUTTON,
                   "the default Apply button must retain dialog-manager semantics");
  passed &= expect(checkbox_type == BS_AUTOCHECKBOX,
                   "the capture checkbox must retain native auto-toggle semantics");
  passed &= expect(visual_checkbox_type == BS_AUTOCHECKBOX,
                   "the visual master checkbox must retain native auto-toggle semantics");

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
  layerc::core::VisualProfile assigned_visual;
  assigned_visual.profile_id = original.id;
  assigned_visual.rules.push_back({.id = "ready", .name = "Ready"});
  assigned_visual.triggers.push_back(
      {.cooldown_id = "dash", .observation_id = "ready"});
  history.reset(original, assigned_visual);
  auto without_cooldown = original;
  without_cooldown.name = "Cooldown removed";
  auto without_visual = assigned_visual;
  without_visual.rules.clear();
  without_visual.triggers.clear();
  passed &= expect(
      history.record(without_cooldown, without_visual, false),
      "a cooldown and visual-assignment removal should share one history transaction");
  const auto restored_bundle =
      history.undo(without_cooldown, without_visual);
  passed &= expect(
      restored_bundle.has_value() &&
          restored_bundle->profile.name == original.name &&
          restored_bundle->visual_profile.has_value() &&
          restored_bundle->visual_profile->triggers.size() == 1,
      "undo should restore a removed cooldown's visual assignment");

  history.reset(original, assigned_visual);
  auto pending_base = original;
  pending_base.name = "Pending base edit";
  passed &= expect(history.record(pending_base, assigned_visual, false),
                    "a pending base edit should enter combined history");
  auto committed_visual = assigned_visual;
  committed_visual.triggers.front().enabled = false;
  passed &= expect(
      history.record(pending_base, committed_visual, false),
      "a committed visual-only action should enter combined history");
  history.rebase_visual(committed_visual);
  passed &= expect(
      history.is_dirty(pending_base, committed_visual, true),
      "committing a visual-only action must not mark a pending base edit saved");
  const auto base_undo = history.undo(pending_base, committed_visual);
  passed &= expect(
      base_undo.has_value() &&
          base_undo->profile.name == pending_base.name &&
          base_undo->visual_profile == assigned_visual,
      "undo should restore the visual state before a visual-only commit");
  if (base_undo.has_value()) {
    const auto visual_redo =
        history.redo(base_undo->profile, base_undo->visual_profile);
    passed &= expect(
        visual_redo.has_value() && visual_redo->profile == pending_base &&
            visual_redo->visual_profile == committed_visual,
        "redo should deterministically restore the committed visual state");
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
  passed &= expect(
      layerc::editor::source_uses_schema_v1(
          R"({"schemaVersion":1,"name":"legacy"})") &&
          !layerc::editor::source_uses_schema_v1(
              R"({"schemaVersion":2,"name":"current"})") &&
          !layerc::editor::source_uses_schema_v1("{broken"),
      "the editor should warn only for a valid schema-v1 source document");

  const HWND editor_dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_EDITOR), nullptr, &test_dialog_proc, 0);
  passed &= expect(editor_dialog != nullptr,
                   "the native V2.1 editor resource should load");
  if (editor_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        editor_dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND assignment = GetDlgItem(editor_dialog, IDC_VISUAL_ENABLED);
    const auto assignment_type =
        GetWindowLongPtrW(assignment, GWL_STYLE) & BS_TYPEMASK;
    passed &= expect(assignment_type == BS_AUTOCHECKBOX,
                     "the visual assignment checkbox must retain auto-toggle semantics");
    SendMessageW(assignment, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(assignment, BM_CLICK, 0, 0);
    passed &= expect(
        SendMessageW(assignment, BM_GETCHECK, 0, 0) == BST_CHECKED,
        "the visual assignment checkbox should toggle through BM_CLICK");
    DestroyWindow(editor_dialog);
  }

  const HWND calibration_dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_CALIBRATION), nullptr, &test_dialog_proc,
      0);
  passed &= expect(
      calibration_dialog != nullptr &&
          GetDlgItem(calibration_dialog, IDC_CAL_CAPTURE_TEMPLATE) != nullptr &&
          GetDlgItem(calibration_dialog, IDC_CAL_IMPORT_TEMPLATE) != nullptr &&
          GetDlgItem(calibration_dialog, IDC_CAL_EDIT_TEMPLATE_MASK) != nullptr,
      "the V2.1 calibration resource should expose capture, import, and mask controls");
  if (calibration_dialog != nullptr) {
    RECT window_rect{};
    GetWindowRect(calibration_dialog, &window_rect);
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(calibration_dialog));
    const int logical_width =
        MulDiv(window_rect.right - window_rect.left, 96, dpi);
    const int logical_height =
        MulDiv(window_rect.bottom - window_rect.top, 96, dpi);
    passed &= expect(
        logical_width <= 1600 && logical_height <= 1040,
        "the calibration dialog should fit inside a 1920x1080 desktop work area at 100% scaling");

    RECT client{};
    GetClientRect(calibration_dialog, &client);
    for (const int id : {IDC_CAL_APPLY, IDCANCEL}) {
      RECT button{};
      const HWND control = GetDlgItem(calibration_dialog, id);
      GetWindowRect(control, &button);
      MapWindowPoints(HWND_DESKTOP, calibration_dialog,
                      reinterpret_cast<POINT *>(&button), 2);
      passed &= expect(button.left >= client.left && button.top >= client.top &&
                           button.right <= client.right &&
                           button.bottom <= client.bottom,
                       "calibration Apply and Cancel must remain inside the visible client area");
    }
    DestroyWindow(calibration_dialog);
  }

  DestroyWindow(dialog);
  return passed ? 0 : 1;
}
