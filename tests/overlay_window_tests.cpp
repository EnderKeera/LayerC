#include "layerc/core/live_edit.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/window_query.hpp"
#include "overlay_collection.hpp"
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

namespace layerc::daemon {
struct OverlayWindowTestAccess {
  static COLORREF pixel(const OverlayWindow &window, const int x, const int y) {
    return window.buffer_dc_ != nullptr ? GetPixel(window.buffer_dc_, x, y) : CLR_INVALID;
  }
};
} // namespace layerc::daemon

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
  const std::wstring command =
      layerc::platform::quote_command_argument(executable);
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
  std::wstring command =
      layerc::platform::quote_command_argument(
          std::wstring_view(executable.data(), length)) +
      L" --desktop-child " +
      layerc::platform::quote_command_argument(test_target);
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
  layerc::core::Panel panel = [] {
    layerc::core::Panel value{};
    value.id = "main";
    value.name = "Main";
    value.style = layerc::core::PanelStyle::card;
    value.bar = {};
    value.anchor_x = 0.5;
    value.anchor_y = 0.75;
    value.cooldown_ids = {"dash"};
    return value;
  }();
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

std::optional<POINT> overlay_pointer_point(const OverlayFixture &fixture) {
  RECT overlay{};
  if (IsWindowVisible(fixture.overlay_window) && IsWindowVisible(fixture.target_window) &&
      !IsIconic(fixture.target_window) && GetWindowRect(fixture.overlay_window, &overlay)) {
    const POINT centre{overlay.left + (overlay.right - overlay.left) / 2,
                       overlay.top + (overlay.bottom - overlay.top) / 2};
    if (PtInRect(&overlay, centre) && PtInRect(&fixture.target_rect, centre) &&
        MonitorFromPoint(centre, MONITOR_DEFAULTTONULL))
      return centre;
  }
  std::cerr << "[skip] overlay pointer probe: no visible overlay centre inside the target client area\n";
  return std::nullopt;
}

void expect_overlay_pointer_target(const OverlayFixture &fixture, const POINT point,
                                   const bool direct_drag, const std::string_view message) {
  const HWND hit = WindowFromPoint(point);
  const HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
  if (!expect(direct_drag ? hit == fixture.overlay_window
                          : hit != fixture.overlay_window && root == fixture.target_window,
              message)) {
    std::cerr << "  point=(" << point.x << ", " << point.y << "), hit=" << hit
              << ", root=" << root << ", overlay=" << fixture.overlay_window
              << ", target=" << fixture.target_window << '\n';
  }
}

void direct_edit_restores_click_through() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  fixture.overlay->show_for(fixture.target_window);
  bool drag_reported{};
  bool drag_final{};
  double dragged_x{};
  int lost_count{};
  std::string lost_panel;
  fixture.overlay->set_edit_mode(true, true, true,
                                 [&](const std::string_view panel_id,
                                     const double anchor_x, const double,
                                     const bool final) {
                                   drag_reported = panel_id == "main";
                                   drag_final = final;
                                   dragged_x = anchor_x;
                                 },
                                 [&](const std::string_view panel_id) {
                                   ++lost_count;
                                   lost_panel = panel_id;
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
  expect(fixture.overlay->direct_drag() && lost_count == 0 &&
             (GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE) &
              WS_EX_TRANSPARENT) == 0,
         "mouse-up completes one drag and keeps Direct drag on");

  OwnedWindow successor{CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"LayerC overlay capture successor",
      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
  if (!expect(successor.window != nullptr, "the same-thread capture successor should exist"))
    return;

  drag_reported = false;
  drag_final = false;
  GetWindowRect(fixture.overlay_window, &overlay_rect);
  const int capture_x = (overlay_rect.right - overlay_rect.left) / 2;
  const int capture_y = (overlay_rect.bottom - overlay_rect.top) / 2;
  SendMessageW(fixture.overlay_window, WM_LBUTTONDOWN, MK_LBUTTON,
               MAKELPARAM(capture_x, capture_y));
  SendMessageW(fixture.overlay_window, WM_MOUSEMOVE, MK_LBUTTON,
               MAKELPARAM(capture_x + 12, capture_y));
  // Take capture away from the overlay without clearing its edit mode.
  expect(GetCapture() == fixture.overlay_window,
         "the overlay owns capture before the transfer");
  SetCapture(successor.window);
  expect(GetCapture() == successor.window,
         "the same-thread window took capture from the overlay");
  expect(drag_reported && drag_final,
         "capture loss should finalize an in-progress direct drag");
  expect(lost_count == 1 && lost_panel == "main",
         "capture loss during a drag reports the end of Direct drag once");
  expect(!fixture.overlay->direct_drag() &&
             (GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE) &
              WS_EX_TRANSPARENT) != 0,
         "capture loss ends Direct drag without a new edit mode");
  if (GetCapture() == successor.window || GetCapture() == fixture.overlay_window)
    ReleaseCapture();
  const auto pointer_point = overlay_pointer_point(fixture);
  if (pointer_point)
    expect_overlay_pointer_target(fixture, *pointer_point, false,
                                  "capture loss gives the point back to the target");
  if (const auto point = pointer_point)
    expect(SendMessageW(fixture.overlay_window, WM_NCHITTEST, 0,
                        MAKELPARAM(point->x, point->y)) == HTTRANSPARENT,
           "after capture loss the overlay is hit-test transparent");

  // Re-armed by its owner, Direct drag takes the pointer again.
  fixture.overlay->set_edit_mode(true, true, true, {}, [&](std::string_view) { ++lost_count; });
  if (pointer_point)
    expect_overlay_pointer_target(fixture, *pointer_point, true,
                                  "direct drag makes the overlay the pointer target");
  fixture.overlay->set_edit_mode(false, false, false);
  expect(lost_count == 1, "turning Direct drag off is not reported as a loss");
  if (pointer_point)
    expect_overlay_pointer_target(fixture, *pointer_point, false,
                                  "ending direct drag gives the same point back to the target");
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

void edit_mode_without_direct_drag_stays_click_through() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  fixture.overlay->show_for(fixture.target_window);
  fixture.overlay->set_edit_mode(true, false, true, [](std::string_view, double, double, bool) {});
  expect((GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0,
         "edit mode without direct drag retains the click-through style");
  if (const auto point = overlay_pointer_point(fixture)) {
    expect(SendMessageW(fixture.overlay_window, WM_NCHITTEST, 0,
                        MAKELPARAM(point->x, point->y)) == HTTRANSPARENT,
           "edit mode without direct drag is hit-test transparent");
    expect_overlay_pointer_target(fixture, *point, false,
                                  "edit mode without direct drag leaves the pointer with the target");
  }
}

void background_overlay_tracks_target() {
  OverlayFixture fixture;
  if (!fixture.show_overlay()) return;
  OwnedWindow unrelated{CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"LayerC background overlay test",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, fixture.work_area.left + 700,
      fixture.work_area.top + 80, 320, 240, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr)};
  expect(unrelated.window != nullptr,
         "an unrelated foreground window should be available");
  if (unrelated.window == nullptr)
    return;
  SetWindowPos(unrelated.window, HWND_TOP, fixture.work_area.left + 700,
               fixture.work_area.top + 80, 320, 240, SWP_SHOWWINDOW);
  SetForegroundWindow(unrelated.window);
  if (!expect(GetAncestor(GetForegroundWindow(), GA_ROOT) != fixture.target_window,
              "the target should be in the background"))
    return;

  const auto expect_aligned = [&](const char *const message) {
    RECT client{};
    POINT origin{};
    RECT overlay_rect{};
    if (!expect(GetClientRect(fixture.target_window, &client) != FALSE &&
                    ClientToScreen(fixture.target_window, &origin) != FALSE &&
                    GetWindowRect(fixture.overlay_window, &overlay_rect) != FALSE,
                "background geometry should be available"))
      return;
    const auto expected = layerc::core::overlay_rectangle(
        {.x = fixture.panel.anchor_x, .y = fixture.panel.anchor_y},
        {origin.x, origin.y, origin.x + client.right - client.left,
         origin.y + client.bottom - client.top},
        overlay_rect.right - overlay_rect.left,
        overlay_rect.bottom - overlay_rect.top);
    expect(IsWindowVisible(fixture.overlay_window) != FALSE &&
               rectangles_near(overlay_rect, expected, 2),
           message);
  };

  // Background updates skip SetWindowPos only when the overlay is already in
  // place, so a moved target and a displaced overlay must both be corrected.
  SetWindowPos(fixture.target_window, nullptr, fixture.work_area.left + 100,
               fixture.work_area.top + 70, 600, 450,
               SWP_NOZORDER | SWP_NOACTIVATE);
  fixture.overlay->show_for(fixture.target_window);
  expect_aligned("a background overlay should follow its moved target");

  RECT displaced{};
  GetWindowRect(fixture.overlay_window, &displaced);
  SetWindowPos(fixture.overlay_window, nullptr, displaced.left + 50,
               displaced.top + 30, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  fixture.overlay->show_for(fixture.target_window);
  expect_aligned("a displaced background overlay should return to its anchor");

  fixture.overlay->show_for(fixture.target_window);
  expect_aligned("an in-place background overlay should stay anchored");
  expect(window_is_above(unrelated.window, fixture.overlay_window),
         "a background overlay should remain below the foreground window");
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

void collection_overrides_survive_recreation() {
  layerc::core::GameProfile profile;
  profile.panels.push_back({.id = "main",
                            .name = "Main",
                            .style = layerc::core::PanelStyle::card,
                            .bar = {},
                            .anchor_x = 0.5,
                            .anchor_y = 0.75,
                            .cooldown_ids = {}});
  layerc::core::OverlayPropertyOverride hidden;
  hidden.name = "hide-main";
  hidden.owner_automation = "automation";
  hidden.panel_id = "main";
  hidden.property = "visible";
  hidden.value = layerc::core::AutomationValue(false);
  auto anchor = hidden;
  anchor.name = "move-main";
  anchor.property = "anchorX";
  anchor.value = layerc::core::AutomationValue(0.8);
  std::vector overrides{hidden, anchor};
  const std::vector<layerc::core::CooldownState> states;
  const auto shown = [&](const layerc::daemon::OverlayCollection &collection) {
    const auto *window = collection.find("main");
    return window != nullptr &&
           window->present(states, layerc::core::MonotonicClock::now()).visible;
  };

  const RECT target{100, 100, 1100, 900};
  const auto check_geometry = [&](layerc::daemon::OverlayCollection &collection,
                                   const double expected_x) {
    // Make the panel visible just for a real window-geometry assertion, then
    // restore the same active visibility override used across recreation.
    overrides.front().value = layerc::core::AutomationValue(true);
    collection.apply_overrides(profile, overrides);
    collection.render_on_rect(target, states,
                              layerc::core::MonotonicClock::now());
    RECT bounds{};
    const auto *window = collection.find("main");
    if (expect(window != nullptr && window->screen_bounds(bounds),
               "the overridden collection should create a visible window")) {
      const auto expected = layerc::core::overlay_rectangle(
          {expected_x, profile.panels.front().anchor_y},
          {target.left, target.top, target.right, target.bottom},
          bounds.right - bounds.left, bounds.bottom - bounds.top);
      expect(rectangles_near(bounds, expected, 2),
             "effective geometry should combine active overrides and base edits");
    }
    overrides.front().value = layerc::core::AutomationValue(false);
    collection.apply_overrides(profile, overrides);
  };

  layerc::daemon::OverlayCollection collection;
  collection.ensure(profile, false);
  collection.apply_overrides(profile, overrides);
  expect(collection.find("main") != nullptr && !shown(collection),
         "an automation override hides the panel");
  check_geometry(collection, 0.8);

  // Overlay demand disappears and returns while the runtime keeps its state.
  collection.release();
  collection.ensure(profile, false);
  collection.apply_overrides(profile, overrides);
  expect(collection.find("main") != nullptr && !shown(collection),
         "a recreated collection keeps the still-active override");
  check_geometry(collection, 0.8);

  profile.panels.front().anchor_x = 0.25;
  profile.panels.front().anchor_y = 0.25;
  collection.rebuild(profile, false);
  collection.apply_overrides(profile, overrides);
  expect(!shown(collection), "a rebuilt base panel keeps the unchanged override");
  check_geometry(collection, 0.8);
  collection.apply_overrides(profile, {});
  expect(shown(collection), "removing the override restores the base panel");
  collection.render_on_rect(target, states,
                            layerc::core::MonotonicClock::now());
  RECT bounds{};
  if (expect(collection.find("main")->screen_bounds(bounds),
             "the base panel should be visible after removing overrides")) {
    const auto expected = layerc::core::overlay_rectangle(
        {0.25, 0.25}, {target.left, target.top, target.right, target.bottom},
        bounds.right - bounds.left, bounds.bottom - bounds.top);
    expect(rectangles_near(bounds, expected, 2),
           "clearing overrides restores the edited base geometry");
  }
}

void typed_rows_render_and_resize() {
  using namespace layerc::core;
  using layerc::daemon::OverlayWindowTestAccess;
  OverlayFixture fixture;
  fixture.panel.style = PanelStyle::bar;
  fixture.panel.bar.show_text = false;
  fixture.cooldowns.front().active_duration = Milliseconds{2'000};
  fixture.panel.cooldown_ids.clear();
  fixture.panel.items = std::vector<OverlayItem>{
      {.id = "cooldown", .label = {}, .content = CooldownItem{"dash"}},
      {.id = "active", .label = {}, .content = CooldownActiveTimerItem{"dash"}},
      {.id = "counter", .label = {},
       .content = CounterItem{"count", 8., CounterDisplay::pips}},
      {.id = "flag", .label = {}, .content = IndicatorItem{"flag", 2.}}};
  AutomationProfile graph;
  graph.profile_id = "fixture";
  graph.variables = {{"count", AutomationValueType::integer, AutomationValue(std::int64_t{2})},
                     {"flag", AutomationValueType::boolean, AutomationValue(true)}};
  const auto automation = automation_default_snapshot(graph);
  CooldownEngine engine(fixture.cooldowns);
  const TimePoint origin{};
  (void)engine.trigger("dash", origin);
  const auto states = engine.snapshot(origin);
  if (!fixture.show_overlay()) return;
  const UINT dpi = GetDpiForWindow(fixture.overlay_window);
  const auto scale = [&](const int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
  const auto pixel = [&](const int x, const int y) {
    return OverlayWindowTestAccess::pixel(*fixture.overlay, scale(x), scale(y));
  };
  auto frame = fixture.overlay->present(states, origin, &automation, origin);
  fixture.overlay->render(frame);
  expect(frame.items.size() == 4, "all four typed item kinds reach GDI");
  const auto expected = panel_geometry(fixture.panel, frame.items.size());
  RECT bounds{};
  GetWindowRect(fixture.overlay_window, &bounds);
  expect(bounds.right - bounds.left == scale(expected.size.width) &&
         bounds.bottom - bounds.top == scale(expected.size.height),
         "typed row count determines the native window size");
  const auto flag = bar_geometry(fixture.panel, 3, 4, 1.).outline;
  const int flag_x = flag.left + 8;
  const int flag_y = (flag.top + flag.bottom) / 2;
  expect(pixel(flag_x, flag_y) == layerc::platform::native_color_ref(frame.items[3].color),
         "an ON indicator paints its resolved accent");
  const auto counter = bar_geometry(fixture.panel, 2, 4, frame.items[2].remaining);
  const auto pips = item_pip_layout(PanelStyle::bar, counter.text, frame.items[2].pips);
  if (expect(pips.pips.size() == 8, "pips reach the native drawing geometry")) {
    const auto sample = [&](const PanelRect &pip) {
      return pixel((pip.left + pip.right) / 2, (pip.top + pip.bottom) / 2);
    };
    expect(sample(pips.pips.front()) == layerc::platform::native_color_ref(frame.items[2].color),
           "a filled pip has the row accent");
    expect(sample(pips.pips.back()) == layerc::platform::native_color_ref(fixture.panel.bar.background_color),
           "an empty pip has a background center, not a solid accent");
  }
  frame = fixture.overlay->present(states, origin + Milliseconds{250}, &automation, origin);
  fixture.overlay->render(frame);
  expect(pixel(flag_x, flag_y) == layerc::platform::native_color_ref(fixture.panel.bar.background_color),
         "the core-computed OFF flash phase clears the accent");
  fixture.overlay->set_session_override({}, {}, {{.item_id = "counter", .visible = false}});
  frame = fixture.overlay->present(states, origin, &automation, origin);
  fixture.overlay->render(frame);
  GetWindowRect(fixture.overlay_window, &bounds);
  expect(frame.items.size() == 3 && bounds.bottom - bounds.top ==
         scale(panel_geometry(fixture.panel, 3).size.height),
         "hiding one item shrinks the native row cache and window");
  fixture.overlay->set_session_override({}, {}, {{.item_id = "flag", .color = 0xFA7B1EU}});
  frame = fixture.overlay->present(states, origin, &automation, origin);
  fixture.overlay->render(frame);
  expect(pixel(flag_x, flag_y) == layerc::platform::native_color_ref(0xFA7B1EU),
         "an item color override reaches the native fill");
  const auto style = GetWindowLongPtrW(fixture.overlay_window, GWL_EXSTYLE);
  expect((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) ==
         (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE) &&
         SendMessageW(fixture.overlay_window, WM_NCHITTEST, 0, 0) == HTTRANSPARENT,
         "typed drawing remains layered, click-through and non-activating");
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
                            LAYERC_TEST_CASE(edit_mode_without_direct_drag_stays_click_through),
                            LAYERC_TEST_CASE(background_overlay_tracks_target),
                            LAYERC_TEST_CASE(target_window_matching),
                            LAYERC_TEST_CASE(collection_overrides_survive_recreation),
                             LAYERC_TEST_CASE(typed_rows_render_and_resize)});
}
