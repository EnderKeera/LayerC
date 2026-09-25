#include "layerc/core/live_edit.hpp"
#include "layerc/platform/window_query.hpp"
#include "overlay_window.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <optional>
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
    return layerc::test::skip_code;
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

std::wstring target_executable;

struct OverlayFixture {
  TargetProcess target{target_executable};
  HWND target_window{};
  RECT work_area{};
  RECT client{};
  POINT origin{};
  RECT target_rect{};
  layerc::core::Panel panel{
      .id = "main",
      .name = "Main",
      .style = layerc::core::PanelStyle::card,
      .bar = {},
      .anchor_x = 0.5,
      .anchor_y = 0.75,
      .cooldown_ids = {"dash"},
  };
  std::vector<layerc::core::CooldownDefinition> cooldowns{
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)}};
  std::optional<layerc::daemon::OverlayWindow> overlay;
  HWND overlay_window{};

  [[nodiscard]] bool prepare_target() {
    if (!expect(target.launched(), "test target should start")) return false;
    if (!expect(wait_for_window(target.id(), L"LCTestWindow", target_window),
                "test target window should appear"))
      return false;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    SetWindowPos(target_window, HWND_TOP, work_area.left + 40, work_area.top + 40,
                 640, 480, SWP_SHOWWINDOW);
    if (!expect(GetClientRect(target_window, &client) != FALSE &&
                    ClientToScreen(target_window, &origin) != FALSE,
                "test target client rectangle should be available"))
      return false;
    target_rect = {origin.x, origin.y,
                   origin.x + client.right - client.left,
                   origin.y + client.bottom - client.top};
    return true;
  }

  [[nodiscard]] bool show_overlay() {
    if (!prepare_target()) return false;
    overlay.emplace(panel, cooldowns);
    overlay->show_on_rect(target_rect);
    return expect(wait_for_window(GetCurrentProcessId(), L"LayerDOverlayWindow",
                                  overlay_window),
                  "overlay window should appear");
  }
};

struct OwnedWindow {
  HWND window{};
  ~OwnedWindow() {
    if (window != nullptr) DestroyWindow(window);
  }
};

void overlay_initial_geometry_and_privacy() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  const UINT global_dpi = layerc::platform::monitor_dpi_for_rect(fixture.target_rect);
  RECT scaled_overlay{};
  GetWindowRect(fixture.overlay_window, &scaled_overlay);
  expect(scaled_overlay.right - scaled_overlay.left ==
                 MulDiv(90, static_cast<int>(global_dpi), 96) &&
             scaled_overlay.bottom - scaled_overlay.top ==
                 MulDiv(84, static_cast<int>(global_dpi), 96),
         "global overlay dimensions should use the target monitor DPI");
  const auto overlay_style = GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE);
  expect((overlay_style & WS_EX_LAYERED) != 0 &&
             (overlay_style & WS_EX_TRANSPARENT) != 0,
         "overlay should use layered transparent hit testing");

  constexpr DWORD capture_exclusion_affinity = 0x00000011;
  DWORD affinity{};
  expect(GetWindowDisplayAffinity(fixture.overlay_window, &affinity) != FALSE &&
             affinity == capture_exclusion_affinity,
         "overlay should exclude itself from supported software capture");
  fixture.overlay->set_capture_exclusion(false);
  expect(GetWindowDisplayAffinity(fixture.overlay_window, &affinity) != FALSE &&
             affinity == WDA_NONE,
         "overlay should allow capture when privacy is disabled");
}

void attached_overlay_follows_target() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  SetWindowPos(fixture.target_window, nullptr,
               fixture.work_area.left + 120, fixture.work_area.top + 90,
               820, 610, SWP_NOZORDER | SWP_NOACTIVATE);
  fixture.origin = {};
  if (!expect(GetClientRect(fixture.target_window, &fixture.client) != FALSE &&
                  ClientToScreen(fixture.target_window, &fixture.origin) != FALSE,
              "resized target geometry should be available"))
    return;
  fixture.overlay->show_for(fixture.target_window);
  RECT aligned_overlay{};
  GetWindowRect(fixture.overlay_window, &aligned_overlay);
  const layerc::core::PixelRect resized_target{
      fixture.origin.x, fixture.origin.y,
      fixture.origin.x + fixture.client.right - fixture.client.left,
      fixture.origin.y + fixture.client.bottom - fixture.client.top};
  const auto expected_overlay = layerc::core::overlay_rectangle(
      {.x = fixture.panel.anchor_x, .y = fixture.panel.anchor_y}, resized_target,
      aligned_overlay.right - aligned_overlay.left,
      aligned_overlay.bottom - aligned_overlay.top);
  RECT resized_monitor_rect{resized_target.left, resized_target.top,
                            resized_target.right, resized_target.bottom};
  const UINT attached_dpi =
      layerc::platform::monitor_dpi_for_rect(resized_monitor_rect);
  expect(aligned_overlay.right - aligned_overlay.left ==
                 MulDiv(90, static_cast<int>(attached_dpi), 96) &&
             aligned_overlay.bottom - aligned_overlay.top ==
                 MulDiv(84, static_cast<int>(attached_dpi), 96),
         "attached overlay dimensions should use the target monitor DPI");
  if (!rectangles_near(aligned_overlay, expected_overlay, 2)) {
    std::cerr << "alignment actual=" << aligned_overlay.left << ','
              << aligned_overlay.top << ',' << aligned_overlay.right << ','
              << aligned_overlay.bottom << " expected=" << expected_overlay.left
              << ',' << expected_overlay.top << ',' << expected_overlay.right
              << ',' << expected_overlay.bottom << '\n';
  }
  expect(rectangles_near(aligned_overlay, expected_overlay, 2),
         "overlay anchors should remain aligned within two pixels after "
         "target move and resize");
}

void direct_edit_restores_click_through() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  fixture.overlay->show_for(fixture.target_window);
  bool drag_reported{};
  bool drag_final{};
  double dragged_x{};
  fixture.overlay->set_edit_mode(true, true, true,
                                 [&](const std::string_view panel_id,
                                     const double anchor_x, const double,
                                     const bool final) {
                                   drag_reported = panel_id == "main";
                                   drag_final = final;
                                   dragged_x = anchor_x;
                                 });
  const auto direct_style = GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE);
  expect((direct_style & WS_EX_TRANSPARENT) == 0 &&
             (direct_style & WS_EX_NOACTIVATE) != 0,
         "direct edit should remove transparency only while remaining "
         "non-activating");

  OwnedWindow unrelated{CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"LayerC overlay z-order test",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, fixture.work_area.left + 80,
      fixture.work_area.top + 80, 480, 320, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr)};
  expect(unrelated.window != nullptr,
         "an unrelated foreground window should be available");
  if (unrelated.window == nullptr)
    return;
  SetWindowPos(unrelated.window, HWND_TOP, fixture.work_area.left + 80,
               fixture.work_area.top + 80, 480, 320, SWP_SHOWWINDOW);
  SetForegroundWindow(unrelated.window);
  fixture.overlay->show_for(fixture.target_window);
  expect(window_is_above(unrelated.window, fixture.overlay_window),
         "a background edit overlay should remain below unrelated windows");
  DestroyWindow(unrelated.window);
  unrelated.window = nullptr;

  RECT overlay_rect{};
  GetWindowRect(fixture.overlay_window, &overlay_rect);
  const int local_x = (overlay_rect.right - overlay_rect.left) / 2;
  const int local_y = (overlay_rect.bottom - overlay_rect.top) / 2;
  SendMessageW(fixture.overlay_window, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(local_x, local_y));
  SendMessageW(fixture.overlay_window, WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(local_x + 24, local_y));
  SendMessageW(fixture.overlay_window, WM_LBUTTONUP, 0,
               MAKELPARAM(local_x + 24, local_y));
  expect(drag_reported && drag_final && dragged_x > 0.5,
         "direct edit should report normalized final bar movement");

  drag_reported = false;
  drag_final = false;
  GetWindowRect(fixture.overlay_window, &overlay_rect);
  const int capture_x = (overlay_rect.right - overlay_rect.left) / 2;
  const int capture_y = (overlay_rect.bottom - overlay_rect.top) / 2;
  SendMessageW(fixture.overlay_window, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(capture_x, capture_y));
  SendMessageW(fixture.overlay_window, WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(capture_x + 12, capture_y));
  SendMessageW(fixture.overlay_window, WM_CAPTURECHANGED, 0, 0);
  if (GetCapture() == fixture.overlay_window)
    ReleaseCapture();
  expect(drag_reported && drag_final,
         "capture loss should finalize an in-progress direct drag");
  fixture.overlay->set_edit_mode(false, false, false);
  expect((GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE) &
          WS_EX_TRANSPARENT) != 0,
         "ending direct edit should restore click-through behavior");

  const POINT overlay_center{
      fixture.origin.x + (fixture.client.right - fixture.client.left) / 2,
      fixture.origin.y + (fixture.client.bottom - fixture.client.top) * 3 / 4,
  };
  const auto hit_result =
      SendMessageW(fixture.overlay_window, WM_NCHITTEST, 0,
                   MAKELPARAM(overlay_center.x, overlay_center.y));
  expect(hit_result == HTTRANSPARENT,
         "overlay should remain hit-test transparent");
}

void target_window_matching() {
  OverlayFixture fixture;
  if (!fixture.prepare_target()) return;
  const auto candidate =
      layerc::platform::describe_attachable_window(fixture.target_window);
  if (!expect(candidate.has_value() && candidate->process_id == fixture.target.id(),
              "window catalog should identify the target process"))
    return;
  layerc::core::GameProfile matching_profile;
  matching_profile.game.executable_name = "lctest.exe";
  expect(layerc::platform::profile_matches_window(*candidate, matching_profile),
         "selected process matcher should match the target");
  auto relocated_candidate = *candidate;
  relocated_candidate.executable_name = L"lctest.exe";
  relocated_candidate.executable_path = L"D:\\Games\\Current\\lctest.exe";
  expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                  matching_profile),
         "name-based application matching should survive a path change");
  matching_profile.game.executable_path = "D:\\Games\\Current\\lctest.exe";
  expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                  matching_profile),
         "exact-path matching should accept the selected executable");
  auto same_name_elsewhere = relocated_candidate;
  same_name_elsewhere.executable_path = L"E:\\Games\\Moved\\lctest.exe";
  expect(!layerc::platform::profile_matches_window(same_name_elsewhere,
                                                   matching_profile),
         "exact-path matching should disambiguate identical names");
  matching_profile.game.executable_name.clear();
  expect(layerc::platform::profile_matches_window(relocated_candidate,
                                                  matching_profile),
         "existing path-only profiles should retain their behavior");
  matching_profile.game.executable_name = "lctest.exe";
  matching_profile.game.executable_path.clear();
  matching_profile.game.window_title = "LayerC test title that cannot match";
  expect(!layerc::platform::profile_matches_window(*candidate,
                                                   matching_profile),
         "additional window title matcher should narrow selection");
}

} // namespace

int wmain(const int argc, wchar_t *argv[]) {
  if (argc == 2)
    return run_on_private_desktop(argv[1]);
  if (argc != 3 || std::wcscmp(argv[1], L"--desktop-child") != 0) {
    std::cerr << "FAIL: lctest executable path is required\n";
    return 1;
  }
  target_executable = argv[2];
  return layerc::test::run({LAYERC_TEST_CASE(overlay_initial_geometry_and_privacy),
                            LAYERC_TEST_CASE(attached_overlay_follows_target),
                            LAYERC_TEST_CASE(direct_edit_restores_click_through),
                            LAYERC_TEST_CASE(target_window_matching)});
}
