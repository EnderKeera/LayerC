#include "editor_prompt.hpp"

#include "editor_resource.h"
#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::editor {
namespace {

constexpr std::array button_ids{IDC_PROMPT_BUTTON1, IDC_PROMPT_BUTTON2,
                                IDC_PROMPT_BUTTON3};

struct PromptState {
  std::wstring_view text;
  std::wstring_view title;
  std::span<const PromptButton> buttons;
  std::size_t default_index{};
  int escape_result{IDCANCEL};
  HFONT font{};
  HBRUSH background{};
  layerc::platform::NativeChromePalette palette{};
};

int scaled(const int logical, const HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  return MulDiv(logical,
                MulDiv(static_cast<int>(dpi == 0 ? 96U : dpi),
                       layerc::platform::native_text_percent(dpi), 100),
                96);
}

// Lays the message out at its measured size: text up to 440 px wide, one
// 32 px button row right-aligned, 16 px padding.
void layout_prompt(const HWND dialog, const PromptState &state) {
  const HWND text = GetDlgItem(dialog, IDC_PROMPT_TEXT);
  const int pad = scaled(20, dialog);
  const int button_height = scaled(32, dialog);
  const int gap = scaled(8, dialog);
  RECT measure{0, 0, scaled(440, dialog), 0};
  const HDC dc = GetDC(dialog);
  const HGDIOBJ previous = SelectObject(dc, state.font);
  DrawTextW(dc, state.text.data(), static_cast<int>(state.text.size()),
            &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
  std::vector<int> widths;
  for (const auto &button : state.buttons) {
    SIZE extent{};
    GetTextExtentPoint32W(dc, button.label.data(),
                          static_cast<int>(button.label.size()), &extent);
    widths.push_back(std::max(scaled(88, dialog), static_cast<int>(extent.cx) + scaled(32, dialog)));
  }
  SelectObject(dc, previous);
  ReleaseDC(dialog, dc);
  int buttons_width = 0;
  for (const int width : widths)
    buttons_width += width + gap;
  buttons_width -= gap;
  const int content_width =
      std::max({static_cast<int>(measure.right), buttons_width,
                scaled(280, dialog)});
  const int client_width = content_width + pad * 2;
  const int client_height =
      pad + static_cast<int>(measure.bottom) + pad + button_height + pad;
  MoveWindow(text, pad, pad, content_width, measure.bottom, FALSE);
  int left = client_width - pad - buttons_width;
  for (std::size_t index = 0; index < button_ids.size(); ++index) {
    const HWND button = GetDlgItem(dialog, button_ids[index]);
    if (index >= state.buttons.size()) {
      ShowWindow(button, SW_HIDE);
      continue;
    }
    MoveWindow(button, left, client_height - pad - button_height,
               widths[index], button_height, FALSE);
    left += widths[index] + gap;
  }
  RECT frame{0, 0, client_width, client_height};
  AdjustWindowRectExForDpi(
      &frame, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)),
      GetDpiForWindow(dialog));
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  RECT anchor{};
  const HWND owner = GetWindow(dialog, GW_OWNER);
  if (owner == nullptr || IsIconic(owner) != FALSE ||
      GetWindowRect(owner, &anchor) == FALSE) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTOPRIMARY),
                    &monitor);
    anchor = monitor.rcWork;
  }
  SetWindowPos(dialog, nullptr,
               anchor.left + (anchor.right - anchor.left - width) / 2,
               anchor.top + (anchor.bottom - anchor.top - height) / 3, width,
               height, SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK prompt_proc(const HWND dialog, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  auto *state =
      reinterpret_cast<PromptState *>(GetWindowLongPtrW(dialog, DWLP_USER));
  switch (message) {
  case WM_INITDIALOG: {
    state = reinterpret_cast<PromptState *>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, lparam);
    state->palette = layerc::platform::native_chrome_palette();
    state->font =
        layerc::platform::create_native_dialog_font(GetDpiForWindow(dialog));
    state->background = CreateSolidBrush(state->palette.menu.fill);
    SetWindowTextW(dialog, std::wstring(state->title).c_str());
    SetDlgItemTextW(dialog, IDC_PROMPT_TEXT, std::wstring(state->text).c_str());
    for (std::size_t index = 0;
         index < state->buttons.size() && index < button_ids.size(); ++index)
      SetDlgItemTextW(dialog, button_ids[index],
                      std::wstring(state->buttons[index].label).c_str());
    layerc::platform::set_dark_title_bar(dialog);
    layerc::platform::set_window_frame_colors(
        dialog, state->palette.menu.fill, state->palette.menu.divider,
        state->palette.menu.text);
    // A prompt is as visible to capture as the window that raised it.
    if (const HWND owner = GetWindow(dialog, GW_OWNER); owner != nullptr) {
      DWORD affinity{};
      if (GetWindowDisplayAffinity(owner, &affinity) != FALSE)
        SetWindowDisplayAffinity(dialog, affinity);
    }
    layerc::platform::style_native_dialog_children(dialog, state->font);
    layout_prompt(dialog, *state);
    const int default_id = button_ids[state->default_index];
    SendMessageW(dialog, DM_SETDEFID, static_cast<WPARAM>(default_id), 0);
    SetFocus(GetDlgItem(dialog, default_id));
    return FALSE;
  }
  case WM_COMMAND: {
    if (state == nullptr)
      break;
    const int id = LOWORD(wparam);
    if (id == IDCANCEL) {
      EndDialog(dialog, state->escape_result);
      return TRUE;
    }
    for (std::size_t index = 0;
         index < state->buttons.size() && index < button_ids.size(); ++index) {
      if (button_ids[index] == id) {
        EndDialog(dialog, state->buttons[index].result);
        return TRUE;
      }
    }
    break;
  }
  case WM_DRAWITEM:
    if (state != nullptr) {
      layerc::platform::paint_native_button(
          *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam),
          state->palette.button, state->font);
      return TRUE;
    }
    break;
  case WM_CTLCOLORDLG:
  case WM_CTLCOLORSTATIC:
    if (state != nullptr) {
      const HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, state->palette.menu.text);
      SetBkColor(dc, state->palette.menu.fill);
      return reinterpret_cast<INT_PTR>(state->background);
    }
    break;
  case WM_DESTROY:
    if (state != nullptr) {
      if (state->font != nullptr)
        DeleteObject(state->font);
      if (state->background != nullptr)
        DeleteObject(state->background);
      state->font = nullptr;
      state->background = nullptr;
    }
    break;
  default:
    break;
  }
  return FALSE;
}

} // namespace

int themed_prompt(const HWND owner, const std::wstring_view text,
                  const std::wstring_view title,
                  const std::span<const PromptButton> buttons,
                  const std::size_t default_index, const int escape_result) {
  if (buttons.empty() || buttons.size() > button_ids.size())
    return escape_result;
  PromptState state{.text = text,
                    .title = title,
                    .buttons = buttons,
                    .default_index = std::min(default_index, buttons.size() - 1),
                    .escape_result = escape_result};
  const INT_PTR result = DialogBoxParamW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROMPT), owner,
      &prompt_proc, reinterpret_cast<LPARAM>(&state));
  return result <= 0 ? escape_result : static_cast<int>(result);
}

int themed_message_box(const HWND owner, const std::wstring_view text,
                       const std::wstring_view title, const UINT type) {
  static constexpr PromptButton ok{L"OK", IDOK};
  static constexpr PromptButton cancel{L"Cancel", IDCANCEL};
  static constexpr PromptButton yes{L"Yes", IDYES};
  static constexpr PromptButton no{L"No", IDNO};
  static constexpr PromptButton retry{L"Retry", IDRETRY};
  std::vector<PromptButton> buttons;
  int escape = IDCANCEL;
  switch (type & MB_TYPEMASK) {
  case MB_OKCANCEL:
    buttons = {ok, cancel};
    break;
  case MB_YESNO:
    buttons = {yes, no};
    escape = IDNO;
    break;
  case MB_YESNOCANCEL:
    buttons = {yes, no, cancel};
    break;
  case MB_RETRYCANCEL:
    buttons = {retry, cancel};
    break;
  default:
    buttons = {ok};
    escape = IDOK;
    break;
  }
  const UINT default_button = type & MB_DEFMASK;
  const std::size_t default_index = default_button == MB_DEFBUTTON2   ? 1
                                    : default_button == MB_DEFBUTTON3 ? 2
                                                                      : 0;
  if ((type & MB_ICONMASK) == MB_ICONERROR ||
      (type & MB_ICONMASK) == MB_ICONWARNING)
    MessageBeep(type & MB_ICONMASK);
  return themed_prompt(owner, text, title, buttons, default_index, escape);
}

} // namespace layerc::editor
