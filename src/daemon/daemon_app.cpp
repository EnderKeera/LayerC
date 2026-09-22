#include "daemon_app.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/platform_events.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_error.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string_view>
#include <vector>

namespace layerc::daemon {

int DaemonApp::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  const auto transaction_scope = layerc::platform::profile_transaction_scope(
      layerc::platform::profile_directory());
  if (!transaction_scope.has_value() ||
      !layerc::platform::recover_profile_transactions(*transaction_scope)) {
    MessageBoxW(
        nullptr,
        L"LayerD could not recover an interrupted profile import. "
        L"Profiles were not loaded to avoid publishing a partial transaction.",
        L"LayerC profile recovery", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (!create_message_window(instance)) {
    return 1;
  }
  load_platform_settings();
  reload_event_ = CreateEventW(nullptr, TRUE, FALSE,
                               layerc::platform::profile_reload_event_name);
  settings_reload_event_ = CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::settings_reload_event_name);
  (void)visual_capture_runtime_.open_activity_event();
  add_tray_icon();
  reload_profiles();
  overlay_runtime_.start();
  SetTimer(window_, game_scan_timer, 1000, nullptr);
  poll_live_channel();

  bool running = true;
  int exit_code{};
  MSG message{};
  while (running) {
    std::array<HANDLE, 5> live_handles{};
    const auto live_handle_count = static_cast<DWORD>(
        live_edit_host_.channel().wait_handles(std::span<HANDLE>(live_handles).first(3)));
    DWORD wait_handle_count = live_handle_count;
    const DWORD gamebar_wait_index = wait_handle_count;
    const HANDLE gamebar_wait = companion_host_.transport().wait_handle();
    if (gamebar_wait != nullptr)
      live_handles[wait_handle_count++] = gamebar_wait;
    const DWORD visual_wait_index = wait_handle_count;
    const bool visual_wait = visual_capture_runtime_.snapshot_event() != nullptr;
    if (visual_wait)
      live_handles[wait_handle_count++] = visual_capture_runtime_.snapshot_event();
    DWORD timeout = INFINITE;
    if (live_edit_host_.session() != nullptr)
      timeout = 250U;
    else if (std::ranges::any_of(visual_capture_runtime_.captures(), [](const auto &entry) {
               return entry.second != nullptr &&
                      entry.second->runtime() ==
                          layerc::core::VisualRuntime::isolated;
             }))
      timeout = 500U;
    else if (wait_handle_count == 0)
      timeout = 1'000U;
    const DWORD result = MsgWaitForMultipleObjectsEx(
        wait_handle_count,
        wait_handle_count == 0 ? nullptr : live_handles.data(), timeout,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) {
      exit_code = 1;
      break;
    }
    if (result == WAIT_TIMEOUT) {
      maintain_visual_captures();
      poll_live_channel();
      continue;
    }
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + live_handle_count) {
      poll_live_channel();
      continue;
    }
    if (gamebar_wait != nullptr &&
        result == WAIT_OBJECT_0 + gamebar_wait_index) {
      poll_gamebar_transport();
      continue;
    }
    if (visual_wait && result == WAIT_OBJECT_0 + visual_wait_index) {
      poll_visual_captures();
      continue;
    }
    if (result != WAIT_OBJECT_0 + wait_handle_count)
      continue;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      if (message.message == WM_QUIT) {
        exit_code = static_cast<int>(message.wParam);
        running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  runtime_kernel_.quiesce();
  overlay_runtime_.stop();
  visual_capture_runtime_.publish_activity(false);
  automation_runtime_host_.publish_activity(false);
  if (reload_event_ != nullptr) {
    CloseHandle(reload_event_);
  }
  if (settings_reload_event_ != nullptr) {
    CloseHandle(settings_reload_event_);
  }
  remove_tray_icon();
  return exit_code;
}

void DaemonApp::load_platform_settings() {
  settings_error_.clear();
  const auto append_error = [&](const std::string_view name,
                                const std::uint32_t error) {
    if (!settings_error_.empty())
      settings_error_ += "; ";
    settings_error_ += name;
    settings_error_ += ": ";
    settings_error_ += layerc::platform::wide_to_utf8(
        layerc::platform::windows_error_text(error));
  };

  const auto capture = layerc::platform::read_capture_exclusion_setting();
  if (capture.value)
    capture_exclusion_ = *capture.value;
  else if (capture.missing())
    capture_exclusion_ = true;
  else
    append_error("capture exclusion", capture.error);

  const auto visual = layerc::platform::read_visual_recognition_setting();
  if (visual.value)
    visual_recognition_enabled_ = *visual.value;
  else if (visual.missing())
    visual_recognition_enabled_ = true;
  else
    append_error("visual recognition", visual.error);
}

bool DaemonApp::create_message_window(const HINSTANCE instance) {
  WNDCLASSW definition{};
  definition.lpfnWndProc = &DaemonApp::window_proc;
  definition.hInstance = instance;
  definition.lpszClassName = L"LayerDMessageWindow";
  if (RegisterClassW(&definition) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  window_ = CreateWindowExW(0, definition.lpszClassName, L"LayerD", 0, 0, 0, 0,
                            0, HWND_MESSAGE, nullptr, instance, this);
  return window_ != nullptr;
}

void DaemonApp::add_tray_icon() {
  tray_ = {};
  tray_.cbSize = sizeof(tray_);
  tray_.hWnd = window_;
  tray_.uID = 1;
  tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  tray_.uCallbackMessage = tray_message;
  tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(tray_.szTip, L"LayerC");
  Shell_NotifyIconW(NIM_ADD, &tray_);
  tray_.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &tray_);
}

void DaemonApp::ensure_tray_icon() {
  if (tray_.hWnd == nullptr) {
    add_tray_icon();
    return;
  }
  auto probe = tray_;
  probe.uFlags = NIF_TIP;
  if (Shell_NotifyIconW(NIM_MODIFY, &probe) == FALSE) {
    add_tray_icon();
  }
}

void DaemonApp::remove_tray_icon() {
  if (tray_.hWnd != nullptr) {
    Shell_NotifyIconW(NIM_DELETE, &tray_);
  }
}

void DaemonApp::show_tray_menu() {
  const HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, visual_capture_runtime_.status().c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, command_open_editor, L"Open editor");
  AppendMenuW(menu, MF_STRING, command_reload, L"Reload profiles");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, command_exit, L"Exit LayerC");
  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                 cursor.x, cursor.y, 0, window_, nullptr);
  DestroyMenu(menu);
}

void DaemonApp::launch_editor() {
  std::vector<wchar_t> module_path(32'768);
  const DWORD length = GetModuleFileNameW(
      nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size()) {
    return;
  }
  const auto editor =
      std::filesystem::path(std::wstring_view(module_path.data(), length))
          .parent_path() /
      L"layere.exe";
  ShellExecuteW(nullptr, L"open", editor.c_str(), nullptr,
                editor.parent_path().c_str(), SW_SHOWNORMAL);
}

void DaemonApp::update_tooltip(const std::wstring &status) {
  if (tray_.hWnd == nullptr) {
    return;
  }
  wcsncpy_s(tray_.szTip, status.c_str(), _TRUNCATE);
  tray_.uFlags = NIF_TIP;
  Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

LRESULT CALLBACK DaemonApp::window_proc(const HWND window, const UINT message,
                                        const WPARAM wparam,
                                        const LPARAM lparam) {
  auto *app =
      reinterpret_cast<DaemonApp *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
    app = static_cast<DaemonApp *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  return app != nullptr ? app->handle_message(window, message, wparam, lparam)
                        : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT DaemonApp::handle_message(const HWND window, const UINT message,
                                  const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_INPUT:
    handle_raw_input(reinterpret_cast<HRAWINPUT>(lparam));
    return 0;
  case WM_INPUT_DEVICE_CHANGE:
    handle_hid_device_change(reinterpret_cast<HANDLE>(lparam),
                             wparam == GIDC_ARRIVAL);
    return 0;
  case WM_TIMER:
    if (wparam == game_scan_timer) {
      publish_companion_snapshots(layerc::core::MonotonicClock::now());
      ensure_tray_icon();
      const bool profiles_reloaded = reload_profiles_if_requested();
      const bool settings_reloaded = reload_settings_if_requested();
      if (!profiles_reloaded && !settings_reloaded) {
        refresh_game_windows();
      }
    } else if (wparam == input_poll_timer) {
      poll_input_devices();
    } else if (wparam == overlay_render_timer) {
      update_overlays();
    } else if (wparam == automation_timer) {
      poll_automation_timer();
    }
    return 0;
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case command_open_editor:
      launch_editor();
      break;
    case command_reload:
      reload_profiles();
      break;
    case command_exit:
      DestroyWindow(window);
      break;
    default:
      break;
    }
    return 0;
  case tray_message:
    if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) {
      show_tray_menu();
    } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
      launch_editor();
    }
    return 0;
  case WM_DESTROY:
    shutting_down_ = true;
    KillTimer(window, game_scan_timer);
    KillTimer(window, overlay_render_timer);
    KillTimer(window, automation_timer);
    end_edit_session();
    runtime_kernel_.quiesce();
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

} // namespace layerc::daemon
