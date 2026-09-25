#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

#include "layerc/core/json.hpp"
#include "layerc/runtime/diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>

#include <windows.h>

namespace layerc::editor {

namespace {

struct DiagnosticsView {
  std::wstring summary;
  bool retry_visual{};
};

// First non-empty last_error among items, or empty when none reported one.
template <typename Items>
std::string first_last_error(const Items &items) {
  const auto found = std::ranges::find_if(
      items, [](const auto &item) { return !item.last_error.empty(); });
  return found == std::ranges::end(items) ? std::string{} : found->last_error;
}

DiagnosticsView diagnostics_view(const std::string_view source,
                                 const bool connected,
                                 const bool authenticated) {
  DiagnosticsView view{};
  view.summary = L"Connection: ";
  if (!connected)
    view.summary += L"LayerD disconnected";
  else if (!authenticated)
    view.summary += L"LayerD connected; authentication pending";
  else
    view.summary += L"LayerD connected and authenticated";

  if (source.empty()) {
    const wchar_t *snapshot_state =
        connected && authenticated ? L"Waiting for snapshot" : L"Unavailable";
    view.summary += L"\r\nActive profile: ";
    view.summary += snapshot_state;
    view.summary += L"\r\nIsolated visual worker: ";
    view.summary += snapshot_state;
    view.summary += connected && authenticated
                        ? L"\r\nLast error: No diagnostics snapshot received"
                        : L"\r\nLast error: LayerD is unavailable";
    return view;
  }

  const auto parsed = layerc::runtime::parse_runtime_diagnostics_json(source);
  const auto *diagnostics =
      std::get_if<layerc::runtime::RuntimeDiagnostics>(&parsed);
  if (diagnostics == nullptr) {
    const auto &error = std::get<layerc::core::json::FieldError>(parsed);
    view.summary += L"\r\nActive profile: Unavailable"
                    L"\r\nIsolated visual worker: Unavailable"
                    L"\r\nLast error: Invalid diagnostics snapshot (";
    view.summary += to_wide(error.describe());
    view.summary += L")";
    return view;
  }

  std::string profile_id;
  for (const auto &session : diagnostics->sessions) {
    if (profile_id.empty() || session.foreground)
      profile_id = session.profile_id;
    if (session.foreground)
      break;
  }
  view.summary += L"\r\nActive profile: ";
  view.summary += profile_id.empty() ? L"None" : to_wide(profile_id);

  const auto helper_count = diagnostics->helpers.size();
  const auto running_count =
      static_cast<std::size_t>(std::ranges::count(
          diagnostics->helpers, true,
          &layerc::runtime::HelperDiagnostics::running));
  const auto quarantined_count =
      static_cast<std::size_t>(std::ranges::count(
          diagnostics->helpers, true,
          &layerc::runtime::HelperDiagnostics::quarantined));
  view.retry_visual = quarantined_count != 0U;
  view.summary += L"\r\nIsolated visual worker: ";
  if (helper_count == 0U) {
    view.summary += L"Not active";
  } else if (quarantined_count != 0U) {
    view.summary += L"Quarantined (" + std::to_wstring(quarantined_count) +
                    L" of " + std::to_wstring(helper_count) + L")";
  } else if (running_count == helper_count) {
    view.summary += L"Running (" + std::to_wstring(helper_count) + L")";
  } else if (running_count == 0U) {
    view.summary += L"Stopped (" + std::to_wstring(helper_count) + L")";
  } else {
    view.summary += L"Degraded (" + std::to_wstring(running_count) + L" of " +
                    std::to_wstring(helper_count) + L" running)";
  }

  std::string last_error = first_last_error(diagnostics->helpers);
  if (last_error.empty())
    last_error = first_last_error(diagnostics->services);
  if (last_error.empty())
    last_error = diagnostics->gamebar.last_error;
  if (last_error.empty())
    last_error = diagnostics->settings.last_error;
  view.summary += L"\r\nLast error: ";
  view.summary += last_error.empty() ? L"None reported" : to_wide(last_error);
  return view;
}

void layout_diagnostics_dialog(const HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const DialogLayout ui(window, window_base_dpi(window));
  const int margin = ui.scaled(12);
  const int gap = ui.scaled(8);
  const int intro_height = ui.scaled(34);
  const int summary_height = ui.scaled(76);
  const int toggle_height = ui.scaled(22);
  const int button_height = ui.scaled(25);
  const int summary_top = margin + intro_height + gap;
  const int toggle_top = summary_top + summary_height + gap;
  const int raw_top = toggle_top + toggle_height + gap;
  const int button_top = height - margin - button_height;

  ui.move(IDC_DIAGNOSTICS_INTRO, margin, margin,
          width - margin * 2, intro_height);
  ui.move(IDC_DIAGNOSTICS_SUMMARY, margin, summary_top,
          width - margin * 2, summary_height);
  ui.move(IDC_DIAGNOSTICS_RAW_TOGGLE, margin, toggle_top,
          ui.scaled(130), toggle_height);
  ui.move(IDC_DIAGNOSTICS_TEXT, margin, raw_top,
          width - margin * 2,
          std::max(1, button_top - gap - raw_top));

  int left = margin;
  ui.next_button(IDC_DIAGNOSTICS_REFRESH, left, button_top, 112,
                 button_height, gap);
  ui.next_button(IDC_DIAGNOSTICS_COPY, left, button_top, 104, button_height,
                 gap);
  if (IsWindowVisible(GetDlgItem(window, IDC_DIAGNOSTICS_RETRY_VISUAL)) != FALSE)
    ui.next_button(IDC_DIAGNOSTICS_RETRY_VISUAL, left, button_top, 92,
                   button_height, gap);
  ui.move(IDOK, width - margin - ui.scaled(72), button_top,
          ui.scaled(72), button_height);
}

void copy_raw_json(const HWND owner, const std::string_view source) {
  if (source.empty() || OpenClipboard(owner) == FALSE)
    return;
  EmptyClipboard();
  const std::wstring text = to_wide(source);
  const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
  const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory != nullptr) {
    void *destination = GlobalLock(memory);
    if (destination != nullptr) {
      std::memcpy(destination, text.c_str(), bytes);
      GlobalUnlock(memory);
      if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
        GlobalFree(memory);
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

} // namespace

void EditorWindow::refresh_runtime_diagnostics_dialog() {
  const HWND dialog = pack_dialogs_.diagnostics_window();
  if (dialog == nullptr || !IsWindow(dialog))
    return;

  const auto &snapshot = pack_dialogs_.runtime_diagnostics_json();
  const bool connected = live_edit_.channel().connected();
  const auto view = diagnostics_view(snapshot, connected,
                                     live_edit_.package_authenticated());
  set_text(GetDlgItem(dialog, IDC_DIAGNOSTICS_SUMMARY), view.summary);

  const bool have_snapshot = !snapshot.empty();
  const HWND toggle = GetDlgItem(dialog, IDC_DIAGNOSTICS_RAW_TOGGLE);
  EnableWindow(toggle, have_snapshot ? TRUE : FALSE);
  const bool show_raw =
      have_snapshot && SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const HWND raw = GetDlgItem(dialog, IDC_DIAGNOSTICS_TEXT);
  set_text(raw, have_snapshot ? to_wide(layerc::core::json::format(snapshot))
                              : std::wstring{});
  ShowWindow(raw, show_raw ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(dialog, IDC_DIAGNOSTICS_COPY),
               have_snapshot ? TRUE : FALSE);
  ShowWindow(GetDlgItem(dialog, IDC_DIAGNOSTICS_RETRY_VISUAL),
             view.retry_visual && connected && live_edit_.package_authenticated()
                 ? SW_SHOW
                 : SW_HIDE);
  layout_diagnostics_dialog(dialog);
}

void EditorWindow::request_runtime_diagnostics() {
  refresh_runtime_diagnostics_dialog();
  if (!live_edit_.channel().connected() || !live_edit_.package_authenticated())
    return;
  live_edit_.advance_revision();
  (void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::diagnostics_request,
      .revision = live_edit_.revision(),
  });
}

void EditorWindow::open_runtime_diagnostics() {
  DialogBoxParamW(shell_.instance(), MAKEINTRESOURCEW(IDD_RUNTIME_DIAGNOSTICS), shell_.window(),
                  &EditorWindow::diagnostics_dialog_proc,
                  reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK EditorWindow::diagnostics_dialog_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  auto *editor = bind_dialog_state<EditorWindow>(window, message, lparam);
  if (editor == nullptr)
    return FALSE;
  if (message == WM_INITDIALOG) {
    editor->pack_dialogs_.diagnostics_window() = window;
    const int dpi = window_base_dpi(window);
    resize_to_logical_size(window, 760, 460);
    apply_dialog_chrome(window, editor->shell_.capture_exclusion());
    if (!editor->pack_dialogs_.diagnostics_chrome().attach(window, static_cast<UINT>(dpi)))
      editor->style_dialog_children(window);
    SendDlgItemMessageW(window, IDC_DIAGNOSTICS_RAW_TOGGLE, BM_SETCHECK,
                        BST_CHECKED, 0);
    editor->request_runtime_diagnostics();
    return TRUE;
  }
  if (message == WM_SIZE) {
    layout_diagnostics_dialog(window);
    return TRUE;
  }
  if (message == WM_DPICHANGED) {
    (void)editor->pack_dialogs_.diagnostics_chrome().on_dpi_changed(HIWORD(wparam), lparam);
    layout_diagnostics_dialog(window);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO) {
    editor->pack_dialogs_.diagnostics_chrome().on_minimum_size(lparam, 520, 320);
    return TRUE;
  }
  if (message == WM_COMMAND) {
    switch (LOWORD(wparam)) {
    case IDC_DIAGNOSTICS_REFRESH:
      editor->request_runtime_diagnostics();
      return TRUE;
    case IDC_DIAGNOSTICS_RAW_TOGGLE:
      editor->refresh_runtime_diagnostics_dialog();
      return TRUE;
    case IDC_DIAGNOSTICS_RETRY_VISUAL:
      if (!editor->live_edit_.channel().connected() ||
          !editor->live_edit_.package_authenticated()) {
        editor->request_runtime_diagnostics();
        return TRUE;
      }
      editor->live_edit_.advance_revision();
      (void)editor->live_edit_.channel().send({
          .type = layerc::core::LiveMessageType::retry_visual,
          .revision = editor->live_edit_.revision(),
      });
      editor->request_runtime_diagnostics();
      return TRUE;
    case IDC_DIAGNOSTICS_COPY:
      copy_raw_json(window, editor->pack_dialogs_.runtime_diagnostics_json());
      return TRUE;
    case IDOK:
    case IDCANCEL:
      EndDialog(window, LOWORD(wparam));
      return TRUE;
    }
  }
  if (message == WM_DESTROY) {
    editor->pack_dialogs_.diagnostics_window() = nullptr;
    editor->pack_dialogs_.diagnostics_chrome().on_destroy();
    return TRUE;
  }
  return editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

} // namespace layerc::editor
