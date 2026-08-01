#include "layerc/platform/windows.hpp"
#include "overlay_window.hpp"

#include <array>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

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

bool expect(const bool condition, const char *const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool window_is_above(const HWND upper, const HWND lower) {
  for (HWND current = GetTopWindow(nullptr); current != nullptr;
       current = GetWindow(current, GW_HWNDNEXT)) {
    if (current == upper) return true;
    if (current == lower) return false;
  }
  return false;
}

} // namespace

int wmain(const int argc, wchar_t *argv[]) {
  if (argc != 2) {
    std::cerr << "FAIL: lctest executable path is required\n";
    return 1;
  }

  TargetProcess target(argv[1]);
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
      .anchor_x = 0.5,
      .anchor_y = 0.75,
      .cooldown_ids = {"dash"},
  };
  const std::vector<layerc::daemon::OverlayCooldown> cooldowns{{
      .id = "dash",
      .name = L"Dash",
      .duration = std::chrono::seconds(5),
  }};
  layerc::daemon::OverlayWindow overlay(panel, cooldowns);
  overlay.show_on_rect(target_rect);

  HWND overlay_window{};
  if (!expect(wait_for_window(GetCurrentProcessId(), L"LayerDOverlayWindow",
                              overlay_window),
              "overlay window should appear")) {
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

  bool drag_reported{};
  bool drag_final{};
  double dragged_x{};
  overlay.set_edit_mode(
      true, true, true,
      [&](const std::string_view panel_id, const double anchor_x,
          const double, const bool final) {
        drag_reported = panel_id == "main";
        drag_final = final;
        dragged_x = anchor_x;
      });
  const auto direct_style = GetWindowLongPtrW(overlay_window, GWL_EXSTYLE);
  if (!expect((direct_style & WS_EX_TRANSPARENT) == 0 &&
                  (direct_style & WS_EX_NOACTIVATE) != 0,
              "direct edit should remove transparency only while remaining non-activating")) {
    return 1;
  }

  const HWND unrelated = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"STATIC", L"LayerC overlay z-order test",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE, work_area.left + 80,
      work_area.top + 80, 480, 320, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!expect(unrelated != nullptr,
              "an unrelated foreground window should be available")) {
    return 1;
  }
  SetWindowPos(unrelated, HWND_TOP, work_area.left + 80, work_area.top + 80,
               480, 320, SWP_SHOWWINDOW);
  SetForegroundWindow(unrelated);
  overlay.show_for(target_window);
  if (!expect(window_is_above(unrelated, overlay_window),
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
  if (GetCapture() == overlay_window) ReleaseCapture();
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
