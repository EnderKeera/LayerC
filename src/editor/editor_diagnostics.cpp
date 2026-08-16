#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

#include <string>

#include <windows.h>

namespace layerc::editor {

void EditorWindow::request_runtime_diagnostics() {
  if (!live_channel_.connected() || !live_package_authenticated_) {
    runtime_diagnostics_json_ =
        "LayerD is not connected. Start or restart LayerD, then Refresh.";
    if (diagnostics_dialog_ != nullptr)
      set_text(GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_TEXT),
               to_wide(runtime_diagnostics_json_));
    return;
  }
  runtime_diagnostics_json_ = "Requesting one-shot diagnostics...";
  if (diagnostics_dialog_ != nullptr)
    set_text(GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_TEXT),
             to_wide(runtime_diagnostics_json_));
  ++revision_;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::diagnostics_request,
      .revision = revision_,
  });
}

void EditorWindow::open_runtime_diagnostics() {
  DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_RUNTIME_DIAGNOSTICS), dialog_,
                  &EditorWindow::diagnostics_dialog_proc,
                  reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK EditorWindow::diagnostics_dialog_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  auto *editor = bind_dialog_state<EditorWindow>(window, message, lparam);
  if (editor == nullptr) return FALSE;
  if (message == WM_INITDIALOG) {
    editor->diagnostics_dialog_ = window;
    set_dark_title_bar(window);
    (void)layerc::platform::apply_capture_exclusion(
        window, editor->capture_exclusion_);
    editor->style_dialog_children(window);
    editor->request_runtime_diagnostics();
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_DIAGNOSTICS_REFRESH:
      editor->request_runtime_diagnostics();
      return TRUE;
    case IDC_DIAGNOSTICS_RETRY_VISUAL:
      if (!editor->live_channel_.connected() ||
          !editor->live_package_authenticated_) {
        editor->request_runtime_diagnostics();
        return TRUE;
      }
      ++editor->revision_;
      (void)editor->live_channel_.send({
          .type = layerc::core::LiveMessageType::retry_visual,
          .revision = editor->revision_,
      });
      editor->request_runtime_diagnostics();
      return TRUE;
    case IDC_DIAGNOSTICS_COPY: {
      const std::wstring text = control_text(
          GetDlgItem(window, IDC_DIAGNOSTICS_TEXT));
      if (OpenClipboard(window) == FALSE) return TRUE;
      EmptyClipboard();
      const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
      const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (memory != nullptr) {
        void *destination = GlobalLock(memory);
        if (destination != nullptr) {
          memcpy(destination, text.c_str(), bytes);
          GlobalUnlock(memory);
          if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
            GlobalFree(memory);
        } else {
          GlobalFree(memory);
        }
      }
      CloseClipboard();
      return TRUE;
    }
    case IDOK:
    case IDCANCEL:
      editor->diagnostics_dialog_ = nullptr;
      EndDialog(window, LOWORD(wparam));
      return TRUE;
    }
  }
  if (message == WM_DRAWITEM) {
    editor->draw_button(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam),
                        layerc::platform::EditorControlRole::button);
    return TRUE;
  }
  if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
    return editor->color_message(message, wparam, lparam);
  if (message == WM_DESTROY) editor->diagnostics_dialog_ = nullptr;
  return FALSE;
}

} // namespace layerc::editor
