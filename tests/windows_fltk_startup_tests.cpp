#include "platform/product_text.hpp"
#include "platform/win32/startup.hpp"
#include "layerc/platform/editor_instance.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "test_support.hpp"

#include <cwchar>
#include <iterator>
#include <string_view>

namespace {
namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;

void shared_guard_name() {
  HANDLE raw_token{};
  TOKEN_STATISTICS statistics{};
  DWORD required{};
  if (!expect(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) != FALSE,
              "can open the current process token"))
    return;
  const platform::OwnedHandle token(raw_token);
  if (!expect(GetTokenInformation(token.get(), TokenStatistics, &statistics,
                                  sizeof(statistics), &required) != FALSE,
              "can read the current logon ID"))
    return;
  wchar_t native_guard[96]{};
  std::swprintf(native_guard, std::size(native_guard),
                L"Local\\LayerC.LayerE.%08lX%08lX.v1",
                static_cast<unsigned long>(statistics.AuthenticationId.HighPart),
                static_cast<unsigned long>(statistics.AuthenticationId.LowPart));
  expect(platform::editor_single_instance_name() == native_guard,
         "the shared name matches the existing Win32 editor's logon mutex");
}

void second_instance_raises_fltk_window() {
  const auto name = L"Local\\LayerC.LayerE.StartupTest." +
                    std::to_wstring(GetCurrentProcessId());
  using State = platform::SingleInstanceGuard::State;
  const platform::SingleInstanceGuard first(name.c_str()), second(name.c_str());
  if (!expect(first.state() == State::acquired &&
              second.state() == State::already_running,
              "a second editor sees the held guard"))
    return;

  WNDCLASSW window_class{};
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.lpszClassName = L"LayerC.LayerE.FLTK";
  if (!expect(RegisterClassW(&window_class) != 0, "the FLTK fixture class is registered"))
    return;
  const HWND window = CreateWindowExW(
      0, window_class.lpszClassName, L"Startup fixture", WS_OVERLAPPEDWINDOW,
      0, 0, 320, 160, nullptr, nullptr, window_class.hInstance, nullptr);
  if (expect(window != nullptr, "the FLTK top-level fixture is created")) {
    ShowWindow(window, SW_SHOWMINNOACTIVE);
    expect(platform::raise_existing_editor(
               [](DWORD, const wchar_t *) { return false; }) == nullptr,
           "a matching window class alone is insufficient");
    const HWND found = platform::raise_existing_editor(
        [](const DWORD process, const wchar_t *image) {
          return process == GetCurrentProcessId() && std::wstring_view(image) == L"layere_fltk.exe";
        });
    expect(found == window && IsIconic(window) == FALSE,
           "the shared raiser finds and restores a matching FLTK window");
    DestroyWindow(window);
  }
  UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
}

void recovery_failure_text() {
  const auto message = editor::recover_startup_profiles({});
  expect(message ==
             "LayerE could not recover an interrupted profile import (profile store scope rejected). "
             "Resolve the _transactions directory before editing profiles.",
         "startup presents the exact recovery reason and next step");
}

void product_text() {
  expect(editor::editor_product_line() == "Windows editor", "Windows product line");
  expect(editor::editor_window_title() == "LayerE — Windows editor", "empty editor title");
  expect(editor::editor_window_title("Sample", false) ==
             "LayerE — Windows editor — Sample", "loaded profile title");
  expect(editor::editor_window_title("Sample", true) ==
             "LayerE — Windows editor — Sample *", "dirty profile title");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(shared_guard_name),
                 LAYERC_TEST_CASE(second_instance_raises_fltk_window),
                 LAYERC_TEST_CASE(recovery_failure_text),
                 LAYERC_TEST_CASE(product_text))
