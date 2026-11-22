#include "layerc/platform/editor_instance.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/platform/window_query.hpp"

#include <cwchar>

namespace layerc::platform {

std::wstring editor_single_instance_name() {
  HANDLE raw_token{};
  TOKEN_STATISTICS statistics{};
  DWORD required{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE) {
    const OwnedHandle token(raw_token);
    if (GetTokenInformation(token.get(), TokenStatistics, &statistics,
                            sizeof(statistics), &required) != FALSE) {
      wchar_t name[96]{};
      swprintf_s(
          name, L"Local\\LayerC.LayerE.%08lX%08lX.v1",
          static_cast<unsigned long>(statistics.AuthenticationId.HighPart),
          static_cast<unsigned long>(statistics.AuthenticationId.LowPart));
      return name;
    }
  }
  return L"Local\\LayerC.LayerE.SingleInstance.v1";
}

HWND raise_existing_editor(const EditorImageMatch matches_image) {
  struct Context {
    EditorImageMatch matches_image;
    std::wstring fltk_class;
    HWND found{};
  } search{matches_image, utf8_to_wide(editor_fltk_window_class)};
  EnumWindows(
      [](const HWND window, const LPARAM raw) -> BOOL {
        auto &context = *reinterpret_cast<Context *>(raw);
        if (GetWindow(window, GW_OWNER) != nullptr)
          return TRUE;
        const auto window_type = window_class(window);
        const wchar_t *filename{};
        if (window_type == L"#32770" &&
            window_text(window).ends_with(L"LayerC Editor")) {
          filename = L"layere.exe";
        } else if (window_type == context.fltk_class && IsWindowVisible(window) &&
                   (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
          filename = L"layere_fltk.exe";
        } else {
          return TRUE;
        }
        DWORD process_id{};
        GetWindowThreadProcessId(window, &process_id);
        if (process_id == 0 || !context.matches_image(process_id, filename))
          return TRUE;
        context.found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  if (search.found != nullptr) {
    if (IsIconic(search.found))
      ShowWindow(search.found, SW_RESTORE);
    ShowWindow(search.found, SW_SHOW);
    SetForegroundWindow(search.found);
  }
  return search.found;
}

void raise_existing_editor() {
  (void)raise_existing_editor(is_sibling_process_image);
}

} // namespace layerc::platform
