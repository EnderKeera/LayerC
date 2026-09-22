#include "layerc/core/live_edit.hpp"
#include "layerc/platform/window_query.hpp"
#include "overlay_window.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using layerc::test::expect;

HWND find_window_for_process(const DWORD process_id,
                             const wchar_t *const class_name) {
  struct Search {
    DWORD process_id;
    const wchar_t *class_name;
    HWND window{};
  } search{process_id, class_name};

  EnumWindows(
      [](const HWND candidate, const LPARAM context) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(context);
        DWORD candidate_process_id{};
        GetWindowThreadProcessId(candidate, &candidate_process_id);
        std::array<wchar_t, 128> candidate_class{};
        GetClassNameW(candidate, candidate_class.data(),
                      static_cast<int>(candidate_class.size()));
        if (candidate_process_id == search.process_id &&
            IsWindowVisible(candidate) &&
            std::wcscmp(candidate_class.data(), search.class_name) == 0) {
          search.window = candidate;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.window;
}

bool wait_for_window(const DWORD process_id, const wchar_t *const class_name,
                     HWND &window) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    window = find_window_for_process(process_id, class_name);
    if (window != nullptr) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool launch_target(const std::wstring &executable,
                   PROCESS_INFORMATION &process) {
  const std::wstring command = L"\"" + executable + L"\"";
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  return CreateProcessW(executable.c_str(), command_buffer.data(), nullptr,
                        nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process) != FALSE;
}

class TargetProcess final {
public:
  explicit TargetProcess(const std::wstring &executable) {
    launched_ = launch_target(executable, process_);
  }

  ~TargetProcess() {
    if (process_.hProcess != nullptr) {
      TerminateProcess(process_.hProcess, 0);
      WaitForSingleObject(process_.hProcess, 1000);
      CloseHandle(process_.hProcess);
    }
    if (process_.hThread != nullptr) {
      CloseHandle(process_.hThread);
    }
  }

  [[nodiscard]] bool launched() const { return launched_; }
  [[nodiscard]] DWORD id() const { return process_.dwProcessId; }

private:
  PROCESS_INFORMATION process_{};
  bool launched_{};
};

int run_on_private_desktop(const wchar_t *const test_target) {
  const std::wstring desktop_name = L"LayerC.OverlayTests." +
                                    std::to_wstring(GetCurrentProcessId()) +
                                    L"." + std::to_wstring(GetTickCount64());
  const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr,
                                       0, GENERIC_ALL, nullptr);
  if (desktop == nullptr) {
    std::cerr
        << "SKIP: private test desktop is unavailable on this window station\n";
    return 77;
  }

  std::vector<wchar_t> executable(32'768);
  const DWORD length = GetModuleFileNameW(
      nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size()) {
    CloseDesktop(desktop);
    return 1;
  }
  std::wstring command = L"\"" + std::wstring(executable.data(), length) +
                         L"\" --desktop-child \"" + test_target + L"\"";
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<LPWSTR>(desktop_name.c_str());
  PROCESS_INFORMATION process{};
  const BOOL launched =
      CreateProcessW(executable.data(), command_buffer.data(), nullptr, nullptr,
                     FALSE, 0, nullptr, nullptr, &startup, &process);
  if (launched == FALSE) {
    CloseDesktop(desktop);
    std::cerr << "FAIL: private-desktop test process could not start\n";
    return 1;
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 30'000);
  DWORD exit_code = 1;
  if (wait == WAIT_OBJECT_0) {
    (void)GetExitCodeProcess(process.hProcess, &exit_code);
  } else {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 1'000);
    std::cerr << "FAIL: private-desktop overlay test timed out\n";
  }
  CloseHandle(process.hProcess);
  CloseDesktop(desktop);
  return static_cast<int>(exit_code);
}

bool window_is_above(const HWND upper, const HWND lower) {
  for (HWND current = GetTopWindow(nullptr); current != nullptr;
       current = GetWindow(current, GW_HWNDNEXT)) {
    if (current == upper)
      return true;
    if (current == lower)
      return false;
  }
  return false;
}

bool rectangles_near(const RECT &left, const layerc::core::PixelRect &right,
                     const LONG tolerance) {
  return std::abs(left.left - right.left) <= tolerance &&
         std::abs(left.top - right.top) <= tolerance &&
         std::abs(left.right - right.right) <= tolerance &&
         std::abs(left.bottom - right.bottom) <= tolerance;
}

} // namespace

int run_overlay_window_tests(const int argc, wchar_t *argv[]) {
  if (argc == 2)
    return run_on_private_desktop(argv[1]);
  if (argc != 3 || std::wcscmp(argv[1], L"--desktop-child") != 0) {
    std::cerr << "FAIL: lctest executable path is required\n";
    return 1;
  }

  TargetProcess target(argv[2]);
  if (!expect(target.launched(), "test target should start")) {
    return 1;
  }

  HWND target_window{};
  if (!expect(wait_for_window(target.id(), L"LCTestWindow", target_window),
              "test target window should appear")) {
    return 1;
  }

  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  SetWindowPos(target_window, HWND_TOP, work_area.left + 40, work_area.top + 40,
               640, 480, SWP_SHOWWINDOW);

  RECT client{};
  POINT origin{};
  if (!expect(GetClientRect(target_window, &client) != FALSE &&
                  ClientToScreen(target_window, &origin) != FALSE,
              "test target client rectangle should be available")) {
    return 1;
  }
  const RECT target_rect{origin.x, origin.y,
                         origin.x + client.right - client.left,
                         origin.y + client.bottom - client.top};
  layerc::core::Panel panel{
      .id = "main",
      .name = "Main",
      .style = layerc::core::PanelStyle::card,
      .bar = {},
      .anchor_x = 0.5,
      .anchor_y = 0.75,
      .cooldown_ids = {"dash"},
  };
  layerc::core::CooldownDefinition cooldown{
      .id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)};
  const std::vector<layerc::core::CooldownDefinition> cooldowns{cooldown};
  layerc::daemon::OverlayWindow overlay(panel, cooldowns);
  overlay.show_on_rect(target_rect);

  HWND overlay_window{};
  if (!expect(wait_for_window(GetCurrentProcessId(), L"LayerDOverlayWindow",
                              overlay_window),
              "overlay window should appear")) {
    return 1;
  }
  const UINT global_dpi = layerc::platform::monitor_dpi_for_rect(target_rect);
  RECT scaled_overlay{};
  GetWindowRect(overlay_window, &scaled_overlay);
  if (!expect(scaled_overlay.right - scaled_overlay.left ==
                      MulDiv(90, static_cast<int>(global_dpi), 96) &&
                  scaled_overlay.bottom - scaled_overlay.top ==
                      MulDiv(84, static_cast<int>(global_dpi), 96),
              "global overlay dimensions should use the target monitor DPI")) {
    return 1;
  }
  const auto overlay_style = GetWindowLongPtrW(overlay_window, GWL_EXSTYLE);
  if (!expect((overlay_style & WS_EX_LAYERED) != 0 &&
                  (overlay_style & WS_EX_TRANSPARENT) != 0,
              "overlay should use layered transparent hit testing")) {
    return 1;
  }

  constexpr DWORD capture_exclusion_affinity = 0x00000011;
  DWORD affinity{};
  if (!expect(
          GetWindowDisplayAffinity(overlay_window, &affinity) != FALSE &&
              affinity == capture_exclusion_affinity,
          "overlay should exclude itself from supported software capture")) {
    return 1;
  }
  overlay.set_capture_exclusion(false);
  if (!expect(GetWindowDisplayAffinity(overlay_window, &affinity) != FALSE &&
                  affinity == WDA_NONE,
              "overlay should allow capture when privacy is disabled")) {
    return 1;
  }

  SetWindowPos(target_window, nullptr, work_area.left + 120, work_area.top + 90,
               820, 610, SWP_NOZORDER | SWP_NOACTIVATE);
  origin = {};
  if (!expect(GetClientRect(target_window, &client) != FALSE &&
                  ClientToScreen(target_window, &origin) != FALSE,
              "resized target geometry should be available")) {
    return 1;
  }
  overlay.show_for(target_window);
  RECT aligned_overlay{};
  GetWindowRect(overlay_window, &aligned_overlay);
  const layerc::core::PixelRect resized_target{
      origin.x, origin.y, origin.x + client.right - client.left,
      origin.y + client.bottom - client.top};
  const auto expected_overlay = layerc::core::overlay_rectangle(
      {.x = panel.anchor_x, .y = panel.anchor_y}, resized_target,
      aligned_overlay.right - aligned_overlay.left,
      aligned_overlay.bottom - aligned_overlay.top);
  RECT resized_monitor_rect{resized_target.left, resized_target.top,
                            resized_target.right, resized_target.bottom};
  const UINT attached_dpi =
      layerc::platform::monitor_dpi_for_rect(resized_monitor_rect);
  if (!expect(
          aligned_overlay.right - aligned_overlay.left ==
                  MulDiv(90, static_cast<int>(attached_dpi), 96) &&
              aligned_overlay.bottom - aligned_overlay.top ==
                  MulDiv(84, static_cast<int>(attached_dpi), 96),
          "attached overlay dimensions should use the target monitor DPI")) {
    return 1;
  }
  if (!rectangles_near(aligned_overlay, expected_overlay, 2)) {
    std::cerr << "alignment actual=" << aligned_overlay.left << ','
              << aligned_overlay.top << ',' << aligned_overlay.right << ','
              << aligned_overlay.bottom << " expected=" << expected_overlay.left
              << ',' << expected_overlay.top << ',' << expected_overlay.right
              << ',' << expected_overlay.bottom << '\n';
  }
  if (!expect(rectangles_near(aligned_overlay, expected_overlay, 2),
              "overlay anchors should remain aligned within two pixels after "
              "target move and resize")) {
    return 1;
  }

  bool drag_reported{};
  bool drag_final{};
  double dragged_x{};
  overlay.set_edit_mode(true, true, true,
                        [&](const std::string_view panel_id,
                            const double anchor_x, const double,
                            const bool final) {
                          drag_reported = panel_id == "main";
                          drag_final = final;
                          dragged_x = anchor_x;
                        });
  const auto direct_style = GetWindowLongPtrW(overlay_window, GWL_EXSTYLE);
  if (!expect((direct_style & WS_EX_TRANSPARENT) == 0 &&
                  (direct_style & WS_EX_NOACTIVATE) != 0,
              "direct edit should remove transparency only while remaining "
              "non-activating")) {
    return 1;
  }

  const HWND unrelated = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"LayerC overlay z-order test",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, work_area.left + 80, work_area.top + 80,
      480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (unrelated == nullptr) {
    (void)expect(false,
                 "an unrelated foreground window should be available");
    return 1;
  }
  SetWindowPos(unrelated, HWND_TOP, work_area.left + 80, work_area.top + 80,
               480, 320, SWP_SHOWWINDOW);
  SetForegroundWindow(unrelated);
  overlay.show_for(target_window);
  if (!expect(
          window_is_above(unrelated, overlay_window),
          "a background edit overlay should remain below unrelated windows")) {
    DestroyWindow(unrelated);
    return 1;
  }
  DestroyWindow(unrelated);

  RECT overlay_rect{};
  GetWindowRect(overlay_window, &overlay_rect);
  const int local_x = (overlay_rect.right - overlay_rect.left) / 2;
  const int local_y = (overlay_rect.bottom - overlay_rect.top) / 2;
  SendMessageW(overlay_window, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(local_x, local_y));
  SendMessageW(overlay_window, WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(local_x + 24, local_y));
  SendMessageW(overlay_window, WM_LBUTTONUP, 0,
               MAKELPARAM(local_x + 24, local_y));
  if (!expect(drag_reported && drag_final && dragged_x > 0.5,
              "direct edit should report normalized final bar movement")) {
    return 1;
  }

  drag_reported = false;
  drag_final = false;
  GetWindowRect(overlay_window, &overlay_rect);
  const int capture_x = (overlay_rect.right - overlay_rect.left) / 2;
  const int capture_y = (overlay_rect.bottom - overlay_rect.top) / 2;
  SendMessageW(overlay_window, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(capture_x, capture_y));
  SendMessageW(overlay_window, WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(capture_x + 12, capture_y));
  SendMessageW(overlay_window, WM_CAPTURECHANGED, 0, 0);
  if (GetCapture() == overlay_window)
    ReleaseCapture();
  if (!expect(drag_reported && drag_final,
              "capture loss should finalize an in-progress direct drag")) {
    return 1;
  }
  overlay.set_edit_mode(false, false, false);
  if (!expect((GetWindowLongPtrW(overlay_window, GWL_EXSTYLE) &
               WS_EX_TRANSPARENT) != 0,
              "ending direct edit should restore click-through behavior")) {
    return 1;
  }

  const auto candidate =
      layerc::platform::describe_attachable_window(target_window);
  if (!expect(candidate.has_value() && candidate->process_id == target.id(),
              "window catalog should identify the target process")) {
    return 1;
  }
  layerc::core::GameProfile matching_profile;
  matching_profile.game.executable_name = "lctest.exe";
  if (!expect(layerc::platform::profile_matches_window(*candidate,
                                                       matching_profile),
              "selected process matcher should match the target")) {
    return 1;
  }
  auto relocated_candidate = *candidate;
  relocated_candidate.executable_name = L"lctest.exe";
  relocated_candidate.executable_path = L"D:\\Games\\Current\\lctest.exe";
  if (!expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                       matching_profile),
              "name-based application matching should survive a path change")) {
    return 1;
  }
  matching_profile.game.executable_path =
      "D:\\Games\\Current\\lctest.exe";
  if (!expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                       matching_profile),
              "exact-path matching should accept the selected executable")) {
    return 1;
  }
  auto same_name_elsewhere = relocated_candidate;
  same_name_elsewhere.executable_path = L"E:\\Games\\Moved\\lctest.exe";
  if (!expect(!layerc::platform::profile_matches_window(same_name_elsewhere,
                                                        matching_profile),
              "exact-path matching should disambiguate identical names")) {
    return 1;
  }
  matching_profile.game.executable_name.clear();
  if (!expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                       matching_profile),
              "existing path-only profiles should retain their behavior")) {
    return 1;
  }
  matching_profile.game.executable_name = "lctest.exe";
  matching_profile.game.executable_path.clear();
  matching_profile.game.window_title = "LayerC test title that cannot match";
  if (!expect(!layerc::platform::profile_matches_window(*candidate,
                                                        matching_profile),
              "additional window title matcher should narrow selection")) {
    return 1;
  }

  const POINT overlay_center{
      origin.x + (client.right - client.left) / 2,
      origin.y + (client.bottom - client.top) * 3 / 4,
  };
  const auto hit_result =
      SendMessageW(overlay_window, WM_NCHITTEST, 0,
                   MAKELPARAM(overlay_center.x, overlay_center.y));
  return expect(hit_result == HTTRANSPARENT,
                "overlay should remain hit-test transparent")
             ? 0
             : 1;
}

int wmain(const int argc, wchar_t *argv[]) {
  return layerc::test::run({{"overlay_window_tests", [argc, argv] {
                              const int result =
                                  run_overlay_window_tests(argc, argv);
                              if (result == 77) layerc::test::skip();
                              layerc::test::expect(
                                  result == 0,
                                  "the overlay program should succeed");
                            }}});
}
