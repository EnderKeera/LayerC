#include "daemon_app.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string_view>

namespace layerc::daemon {

int DaemonApp::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  if (!layerc::platform::recover_profile_transactions(
          layerc::platform::profile_directory())) {
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
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  visual_recognition_enabled_ = layerc::platform::visual_recognition_enabled();
  reload_event_ = CreateEventW(nullptr, TRUE, FALSE,
                               layerc::platform::profile_reload_event_name);
  settings_reload_event_ = CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::settings_reload_event_name);
  visual_capture_active_event_ = CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::visual_capture_active_event_name);
  add_tray_icon();
  reload_profiles();
  foreground_hook_target_ = this;
  foreground_event_hook_ = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &DaemonApp::foreground_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  // Include destroy notifications so a closed matched game immediately retires
  // its instance and owned overlays rather than waiting for the next scan.
  location_event_hook_ = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
      &DaemonApp::foreground_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetTimer(window_, game_scan_timer, 1000, nullptr);
  poll_live_channel();

  bool running = true;
  int exit_code{};
  MSG message{};
  while (running) {
    std::array<HANDLE, 5> live_handles{};
    const auto live_handle_count = static_cast<DWORD>(
        live_channel_.wait_handles(std::span<HANDLE>(live_handles).first(3)));
    DWORD wait_handle_count = live_handle_count;
    const DWORD gamebar_wait_index = wait_handle_count;
    const HANDLE gamebar_wait = gamebar_transport_.wait_handle();
    if (gamebar_wait != nullptr)
      live_handles[wait_handle_count++] = gamebar_wait;
    const DWORD visual_wait_index = wait_handle_count;
    const bool visual_wait = visual_snapshot_event_ != nullptr;
    if (visual_wait)
      live_handles[wait_handle_count++] = visual_snapshot_event_;
    const DWORD timeout =
        edit_session_ != nullptr
            ? 250U
            : (!isolated_visual_captures_.empty()
                   ? 500U
                   : (wait_handle_count == 0 ? 1'000U : INFINITE));
    const DWORD result = MsgWaitForMultipleObjectsEx(
        wait_handle_count,
        wait_handle_count == 0 ? nullptr : live_handles.data(), timeout,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) {
      exit_code = 1;
      break;
    }
    if (result == WAIT_TIMEOUT) {
      maintain_isolated_visual_captures();
      poll_live_channel();
      gamebar_transport_.poll();
      continue;
    }
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + live_handle_count) {
      poll_live_channel();
      continue;
    }
    if (gamebar_wait != nullptr &&
        result == WAIT_OBJECT_0 + gamebar_wait_index) {
      gamebar_transport_.poll();
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

  if (foreground_hook_target_ == this) {
    foreground_hook_target_ = nullptr;
  }
  if (foreground_event_hook_ != nullptr) {
    UnhookWinEvent(foreground_event_hook_);
  }
  if (location_event_hook_ != nullptr) {
    UnhookWinEvent(location_event_hook_);
  }
  if (reload_event_ != nullptr) {
    CloseHandle(reload_event_);
  }
  if (settings_reload_event_ != nullptr) {
    CloseHandle(settings_reload_event_);
  }
  if (visual_capture_active_event_ != nullptr) {
    CloseHandle(visual_capture_active_event_);
  }
  if (automation_active_event_ != nullptr) {
    CloseHandle(automation_active_event_);
    automation_active_event_ = nullptr;
  }
  remove_tray_icon();
  return exit_code;
}

bool DaemonApp::create_message_window(const HINSTANCE instance) {
  const WNDCLASSW definition{
      .lpfnWndProc = &DaemonApp::window_proc,
      .hInstance = instance,
      .lpszClassName = L"LayerDMessageWindow",
  };
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
  AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, visual_status_.c_str());
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
  std::array<wchar_t, 32'768> module_path{};
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
      gamebar_transport_.poll();
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
