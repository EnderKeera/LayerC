#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

#include "layerc/core/json.hpp"

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

std::string pretty_json_whitespace(const std::string_view source) {
  std::string output;
  output.reserve(source.size() + source.size() / 2U);
  int indent{};
  bool in_string{};
  bool escaped{};
  const auto new_line = [&] {
    output.push_back('\n');
    output.append(static_cast<std::size_t>(indent) * 2U, ' ');
  };

  for (const char character : source) {
    if (in_string) {
      output.push_back(character);
      if (escaped)
        escaped = false;
      else if (character == '\\')
        escaped = true;
      else if (character == '"')
        in_string = false;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0)
      continue;
    switch (character) {
    case '"':
      in_string = true;
      output.push_back(character);
      break;
    case '{':
    case '[':
      output.push_back(character);
      ++indent;
      new_line();
      break;
    case '}':
    case ']': {
      indent = std::max(0, indent - 1);
      const char opening = character == '}' ? '{' : '[';
      const auto previous = output.find_last_not_of(" \t\r\n");
      if (previous != std::string::npos && output[previous] == opening)
        output.resize(previous + 1U);
      else
        new_line();
      output.push_back(character);
      break;
    }
    case ',':
      output.push_back(character);
      new_line();
      break;
    case ':':
      output += ": ";
      break;
    default:
      output.push_back(character);
      break;
    }
  }
  return output;
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

  const auto parsed = layerc::core::json::parse(source);
  const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
  if (root == nullptr || !root->is_object()) {
    view.summary += L"\r\nActive profile: Unavailable"
                    L"\r\nIsolated visual worker: Unavailable"
                    L"\r\nLast error: Diagnostics snapshot is not valid JSON";
    return view;
  }

  std::string profile_id;
  if (const auto *sessions = root->find("sessions");
      sessions != nullptr && sessions->is_array()) {
    for (const auto &session : sessions->as_array()) {
      if (!session.is_object())
        continue;
      const auto *id = session.find("profileId");
      if (id == nullptr || !id->is_string())
        continue;
      const auto *foreground = session.find("foreground");
      const bool is_foreground = foreground != nullptr &&
                                 foreground->is_bool() && foreground->as_bool();
      if (profile_id.empty() || is_foreground)
        profile_id = id->as_string();
      if (is_foreground)
        break;
    }
  }
  view.summary += L"\r\nActive profile: ";
  view.summary += profile_id.empty() ? L"None" : to_wide(profile_id);

  std::size_t helper_count{};
  std::size_t running_count{};
  std::size_t quarantined_count{};
  std::string last_error;
  if (const auto *helpers = root->find("isolatedHelpers");
      helpers != nullptr && helpers->is_array()) {
    for (const auto &helper : helpers->as_array()) {
      if (!helper.is_object())
        continue;
      ++helper_count;
      const auto *running = helper.find("running");
      if (running != nullptr && running->is_bool() && running->as_bool())
        ++running_count;
      const auto *quarantined = helper.find("quarantined");
      if (quarantined != nullptr && quarantined->is_bool() &&
          quarantined->as_bool())
        ++quarantined_count;
      const auto *error = helper.find("lastError");
      if (last_error.empty() && error != nullptr && error->is_string() &&
          !error->as_string().empty())
        last_error = error->as_string();
    }
  }
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

  if (last_error.empty()) {
    if (const auto *services = root->find("services");
        services != nullptr && services->is_array()) {
      for (const auto &service : services->as_array()) {
        if (!service.is_object())
          continue;
        const auto *error = service.find("lastError");
        if (error != nullptr && error->is_string() &&
            !error->as_string().empty()) {
          last_error = error->as_string();
          break;
        }
      }
    }
  }
  if (last_error.empty()) {
    const auto *transport = root->find("gameBarTransport");
    const auto *error = transport != nullptr && transport->is_object()
                            ? transport->find("lastError")
                            : nullptr;
    if (error != nullptr && error->is_string())
      last_error = error->as_string();
  }
  view.summary += L"\r\nLast error: ";
  view.summary += last_error.empty() ? L"None reported" : to_wide(last_error);
  return view;
}

void layout_diagnostics_dialog(const HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const int dpi = window_base_dpi(window);
  const auto scaled = [dpi](const int value) { return dpi_scaled(value, dpi); };
  const int margin = scaled(12);
  const int gap = scaled(8);
  const int intro_height = scaled(34);
  const int summary_height = scaled(76);
  const int toggle_height = scaled(22);
  const int button_height = scaled(25);
  const int summary_top = margin + intro_height + gap;
  const int toggle_top = summary_top + summary_height + gap;
  const int raw_top = toggle_top + toggle_height + gap;
  const int button_top = height - margin - button_height;

  move_control(window, IDC_DIAGNOSTICS_INTRO, margin, margin,
                        width - margin * 2, intro_height);
  move_control(window, IDC_DIAGNOSTICS_SUMMARY, margin, summary_top,
                        width - margin * 2, summary_height);
  move_control(window, IDC_DIAGNOSTICS_RAW_TOGGLE, margin, toggle_top,
                        scaled(130), toggle_height);
  move_control(window, IDC_DIAGNOSTICS_TEXT, margin, raw_top,
                        width - margin * 2,
                        std::max(1, button_top - gap - raw_top));

  int left = margin;
  const auto move_button = [&](const int id, const int logical_width) {
    move_control(window, id, left, button_top, scaled(logical_width),
                          button_height);
    left += scaled(logical_width) + gap;
  };
  move_button(IDC_DIAGNOSTICS_REFRESH, 112);
  move_button(IDC_DIAGNOSTICS_COPY, 104);
  if (IsWindowVisible(GetDlgItem(window, IDC_DIAGNOSTICS_RETRY_VISUAL)) != FALSE)
    move_button(IDC_DIAGNOSTICS_RETRY_VISUAL, 92);
  move_control(window, IDOK, width - margin - scaled(72), button_top,
                        scaled(72), button_height);
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
  if (diagnostics_dialog_ == nullptr || !IsWindow(diagnostics_dialog_))
    return;

  const bool connected = live_channel_.connected();
  const auto view = diagnostics_view(runtime_diagnostics_json_, connected,
                                     live_package_authenticated_);
  set_text(GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_SUMMARY),
           view.summary);

  const bool have_snapshot = !runtime_diagnostics_json_.empty();
  const HWND toggle =
      GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_RAW_TOGGLE);
  EnableWindow(toggle, have_snapshot ? TRUE : FALSE);
  const bool show_raw =
      have_snapshot && SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const HWND raw = GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_TEXT);
  set_text(raw, have_snapshot
                    ? to_wide(pretty_json_whitespace(runtime_diagnostics_json_))
                    : std::wstring{});
  ShowWindow(raw, show_raw ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_COPY),
               have_snapshot ? TRUE : FALSE);
  ShowWindow(GetDlgItem(diagnostics_dialog_, IDC_DIAGNOSTICS_RETRY_VISUAL),
             view.retry_visual && connected && live_package_authenticated_
                 ? SW_SHOW
                 : SW_HIDE);
  layout_diagnostics_dialog(diagnostics_dialog_);
}

void EditorWindow::request_runtime_diagnostics() {
  refresh_runtime_diagnostics_dialog();
  if (!live_channel_.connected() || !live_package_authenticated_)
    return;
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
  if (editor == nullptr)
    return FALSE;
  if (message == WM_INITDIALOG) {
    editor->diagnostics_dialog_ = window;
    const int dpi = window_base_dpi(window);
    resize_to_logical_size(window, 760, 460);
    set_dark_title_bar(window);
    (void)layerc::platform::apply_capture_exclusion(
        window, editor->capture_exclusion_);
    editor->diagnostics_font_ = create_editor_font(static_cast<UINT>(dpi));
    style_native_dialog_children(
        window, editor->diagnostics_font_ == nullptr ? editor->font_
                                                     : editor->diagnostics_font_);
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
    apply_suggested_dpi_rect(window, lparam);
    const HFONT replacement = create_editor_font(HIWORD(wparam));
    if (replacement != nullptr) {
      const HFONT previous = editor->diagnostics_font_;
      editor->diagnostics_font_ = replacement;
      style_native_dialog_children(window, replacement);
      if (previous != nullptr)
        DeleteObject(previous);
    }
    layout_diagnostics_dialog(window);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO) {
    apply_minimum_track_size(window, lparam, 520, 320);
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
    case IDC_DIAGNOSTICS_COPY:
      copy_raw_json(window, editor->runtime_diagnostics_json_);
      return TRUE;
    case IDOK:
    case IDCANCEL:
      EndDialog(window, LOWORD(wparam));
      return TRUE;
    }
  }
  if (message == WM_DESTROY) {
    editor->diagnostics_dialog_ = nullptr;
    if (editor->diagnostics_font_ != nullptr) {
      DeleteObject(editor->diagnostics_font_);
      editor->diagnostics_font_ = nullptr;
    }
    return TRUE;
  }
  return editor->dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

} // namespace layerc::editor
