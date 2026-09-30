#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

struct PromptButton {
  std::wstring_view label;
  int result{};
};

// The editor's one themed modal prompt: dark surface, the shared owner-drawn
// buttons, sized to its text and centered on `owner`. Esc returns
// `escape_result`; Enter activates the default button.
int themed_prompt(HWND owner, std::wstring_view text, std::wstring_view title,
                  std::span<const PromptButton> buttons,
                  std::size_t default_index = 0, int escape_result = IDCANCEL);

// MessageBoxW-compatible replacement (MB_OK, MB_OKCANCEL, MB_YESNO,
// MB_YESNOCANCEL, MB_RETRYCANCEL, MB_DEFBUTTONn) returning IDOK/IDYES/...
int themed_message_box(HWND owner, std::wstring_view text,
                       std::wstring_view title, UINT type);

} // namespace layerc::editor
