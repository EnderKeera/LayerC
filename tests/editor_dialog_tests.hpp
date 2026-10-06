#pragma once

#include <windows.h>

namespace layerc::test::editor_dialog {

// These checks share the original Options fixture, assertion accumulator, and
// execution order through editor_dialog_contracts.
INT_PTR CALLBACK test_dialog_proc(HWND window, UINT message, WPARAM wparam,
                                 LPARAM lparam);

void profile_history_contracts(bool &passed);
void profile_checkbox_contracts(bool &passed);
void source_schema_contracts(bool &passed);
void appearance_draft_contracts(bool &passed);
void automation_preset_contracts(bool &passed);
void cooldown_edit_contracts(bool &passed);
void main_dialog_contracts(HINSTANCE instance, bool &passed);
void appearance_dialog_contracts(HINSTANCE instance, bool &passed);
void tasks_dialog_contracts(HINSTANCE instance, bool &passed);
void pack_dialog_contracts(HINSTANCE instance, bool &passed);
void automation_dialog_contracts(HINSTANCE instance, bool &passed);
void hid_dialog_contracts(HINSTANCE instance, bool &passed);
void diagnostics_dialog_contracts(HINSTANCE instance, bool &passed);
void task_picker_contracts(bool &passed);
void panel_preview_contracts(bool &passed);
void binding_capture_contracts(bool &passed);

} // namespace layerc::test::editor_dialog
