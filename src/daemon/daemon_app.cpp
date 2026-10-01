#include "daemon_app.hpp"
#include "maintenance_schedule.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/platform_events.hpp"
#include "layerc/platform/settings_store.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_error.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string_view>

namespace layerc::daemon {

int DaemonApp::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  const auto recovered = layerc::platform::recover_profile_transactions(
      layerc::platform::profile_directory());
  if (const auto error = recovered.error(); error.has_value()) {
    const auto message =
        L"LayerD could not recover an interrupted profile import (" +
        layerc::platform::utf8_to_wide(
            layerc::platform::profile_write_error_text(*error)) +
        L"). Profiles were not loaded to avoid publishing a partial "
        L"transaction.";
    MessageBoxW(nullptr, message.c_str(), L"LayerC profile recovery",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  if (!create_message_window(instance)) {
    return 1;
  }
  load_platform_settings();
  reload_event_.reset(CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::profile_reload_event_name));
  settings_reload_event_.reset(CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::settings_reload_event_name));
  (void)visual_capture_runtime_.open_activity_event();
  add_tray_icon();
  reload_profiles();
  overlay_runtime_.start();
  SetTimer(window_, game_scan_timer, 1000, nullptr);
  poll_live_channel();

  bool running = true;
  int exit_code{};
  MSG message{};
  MaintenanceSchedule maintenance;
  while (running) {
    // Flush here for work done outside window messages (loop-level polls,
    // WinEvent callbacks); handle_message flushes for modal-loop dispatch.
    flush_overlays();
    std::array<HANDLE, 5> live_handles{};
    const auto live_handle_count = static_cast<DWORD>(
        live_edit_host_.channel.wait_handles(std::span<HANDLE>(live_handles).first(3)));
    DWORD wait_handle_count = live_handle_count;
    const DWORD gamebar_wait_index = wait_handle_count;
    const HANDLE gamebar_wait = companion_host_.transport.wait_handle();
    if (gamebar_wait != nullptr)
      live_handles[wait_handle_count++] = gamebar_wait;
    const DWORD visual_wait_index = wait_handle_count;
    const bool visual_wait = visual_capture_runtime_.snapshot_event() != nullptr;
    if (visual_wait)
      live_handles[wait_handle_count++] = visual_capture_runtime_.snapshot_event();
    std::optional<layerc::core::Milliseconds> interval;
    if (live_edit_host_.session != nullptr)
      interval = layerc::core::Milliseconds{250};
    else if (std::ranges::any_of(visual_capture_runtime_.captures, [](const auto &entry) {
               return entry.second != nullptr &&
                      entry.second->runtime() ==
                          layerc::core::VisualRuntime::isolated;
             }))
      interval = layerc::core::Milliseconds{500};
    else if (wait_handle_count == 0)
      interval = layerc::core::Milliseconds{1'000};
    const auto wait =
        maintenance.wait_for(layerc::core::MonotonicClock::now(), interval);
    const DWORD timeout =
        wait ? static_cast<DWORD>(wait->count()) : INFINITE;
    const DWORD result = MsgWaitForMultipleObjectsEx(
        wait_handle_count,
        wait_handle_count == 0 ? nullptr : live_handles.data(), timeout,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) {
      exit_code = 1;
      break;
    }
    // Watchdog and lease checks run on their deadline after any wake-up, so
    // continuous input, timers, or snapshots cannot starve them.
    if (maintenance.take_due(layerc::core::MonotonicClock::now())) {
      maintain_visual_captures();
      poll_live_channel();
    }
    if (result == WAIT_TIMEOUT)
      continue;
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
    // Return to the deadline/handle checks even if producers keep the queue
    // non-empty. Draining without a bound can starve maintenance just as a
    // timeout-only check can.
    for (unsigned dispatched = 0;
         dispatched < 64 &&
         PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE;
         ++dispatched) {
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
  reload_event_.reset();
  settings_reload_event_.reset();
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
  // Both flags default to enabled when the setting is absent from the store.
  const auto apply = [&](bool &target, const auto &result,
                         const std::string_view name) {
    if (result.value)
      target = *result.value;
    else if (result.missing())
      target = true;
    else
      append_error(name, result.error);
  };

  apply(capture_exclusion_, layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::capture_exclusion),
        "capture exclusion");
  apply(visual_recognition_enabled_,
        layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::visual_recognition),
        "visual recognition");
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
  // The LayerC icon from layerd.rc; stock icon only if the resource is missing.
  const int icon_size = GetSystemMetrics(SM_CXSMICON);
  tray_.hIcon = static_cast<HICON>(LoadImageW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, icon_size,
      icon_size, LR_DEFAULTCOLOR | LR_SHARED));
  if (tray_.hIcon == nullptr)
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
  AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, visual_capture_runtime_.status.c_str());
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
  const auto editor = platform::sibling_image_path(L"layere.exe");
  if (editor.empty()) {
    return;
  }
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
  const LRESULT result = dispatch_message(window, message, wparam, lparam);
  flush_overlays();
  return result;
}

LRESULT DaemonApp::dispatch_message(const HWND window, const UINT message,
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
      mark_overlays_dirty();
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
