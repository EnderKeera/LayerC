#include "daemon_app.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

#include <shlobj.h>

namespace layerc::daemon {
namespace {

constexpr UINT tray_message = WM_APP + 1;
constexpr UINT_PTR game_scan_timer = 1;
constexpr UINT_PTR input_poll_timer = 2;
constexpr UINT_PTR overlay_render_timer = 3;
constexpr UINT command_open_editor = 1001;
constexpr UINT command_reload = 1002;
constexpr UINT command_exit = 1003;
constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";

DaemonApp *foreground_hook_target{};

std::wstring utf8_to_wide(const std::string_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), size) == 0) {
    return {};
  }
  return result;
}

std::wstring lower_copy(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const wchar_t character) {
                   return static_cast<wchar_t>(std::towlower(character));
                 });
  return value;
}

std::wstring process_path_for_window(const HWND window) {
  DWORD process_id{};
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == 0) {
    return {};
  }
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     process_id);
  if (process == nullptr) {
    return {};
  }
  std::array<wchar_t, 32'768> path{};
  DWORD length = static_cast<DWORD>(path.size());
  const BOOL succeeded =
      QueryFullProcessImageNameW(process, 0, path.data(), &length);
  CloseHandle(process);
  return succeeded ? std::wstring(path.data(), length) : std::wstring{};
}

std::wstring window_text(const HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(window, result.data(), static_cast<int>(result.size()));
  result.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return result;
}

std::wstring window_class(const HWND window) {
  std::array<wchar_t, 256> value{};
  const int copied =
      GetClassNameW(window, value.data(), static_cast<int>(value.size()));
  return copied > 0
             ? std::wstring(value.data(), static_cast<std::size_t>(copied))
             : std::wstring{};
}

bool matches_profile(const HWND window,
                     const layerc::core::GameProfile &profile) {
  if (profile.game.global || !profile.enabled) {
    return false;
  }
  const auto process_path = lower_copy(process_path_for_window(window));
  if (process_path.empty()) {
    return false;
  }
  const auto expected_path =
      lower_copy(utf8_to_wide(profile.game.executable_path));
  if (!expected_path.empty() && process_path != expected_path) {
    return false;
  }
  const auto expected_name =
      lower_copy(utf8_to_wide(profile.game.executable_name));
  if (!expected_name.empty()) {
    const auto actual_name =
        lower_copy(std::filesystem::path(process_path).filename().wstring());
    if (actual_name != expected_name) {
      return false;
    }
  }
  const auto expected_class =
      lower_copy(utf8_to_wide(profile.game.window_class));
  if (!expected_class.empty() &&
      lower_copy(window_class(window)) != expected_class) {
    return false;
  }
  const auto expected_title =
      lower_copy(utf8_to_wide(profile.game.window_title));
  return expected_title.empty() ||
         lower_copy(window_text(window)).find(expected_title) !=
             std::wstring::npos;
}

std::uint64_t visible_area(const HWND window) {
  if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window) ||
      IsIconic(window) || GetAncestor(window, GA_ROOT) != window ||
      GetWindow(window, GW_OWNER) != nullptr) {
    return 0;
  }
  RECT rectangle{};
  if (!GetClientRect(window, &rectangle)) {
    return 0;
  }
  const auto width = std::max<LONG>(0, rectangle.right - rectangle.left);
  const auto height = std::max<LONG>(0, rectangle.bottom - rectangle.top);
  return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
}

std::vector<OverlayCooldown> make_overlay_cooldowns(
    const std::vector<layerc::core::CooldownDefinition> &definitions) {
  std::vector<OverlayCooldown> result;
  result.reserve(definitions.size());
  for (const auto &definition : definitions) {
    result.push_back({.id = definition.id,
                      .name = utf8_to_wide(definition.name),
                      .duration = definition.duration});
  }
  return result;
}

std::optional<std::string> read_profile_file(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > layerc::core::max_profile_json_bytes) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
    return std::nullopt;
  }
  return contents;
}

} // namespace

DaemonApp::Session::Session(layerc::core::GameProfile source_profile,
                            std::wstring source_key_value,
                            const bool capture_exclusion)
    : source_key(std::move(source_key_value)),
      profile(std::move(source_profile)),
      overlay_cooldowns(make_overlay_cooldowns(profile.cooldowns)),
      cooldowns(profile.cooldowns), input(profile.bindings) {
  snapshot_buffer.reserve(profile.cooldowns.size());
  for (const auto &binding : profile.bindings) {
    for (const auto &token : binding.chord.tokens) {
      uses_controller = uses_controller ||
                        token.device == layerc::core::InputDevice::controller;
      if (!is_global() || token.device != layerc::core::InputDevice::keyboard) {
        continue;
      }
      const auto existing = std::find_if(
          global_keyboard.begin(), global_keyboard.end(),
          [&](const PolledKeyState &key) { return key.token == token; });
      if (existing == global_keyboard.end()) {
        global_keyboard.push_back({.token = token});
      }
    }
  }

  overlays.reserve(profile.panels.size());
  for (const auto &panel : profile.panels) {
    auto overlay = std::make_unique<OverlayWindow>(panel, overlay_cooldowns);
    overlay->set_capture_exclusion(capture_exclusion);
    overlays.push_back(std::move(overlay));
  }
}

void DaemonApp::Session::set_capture_exclusion(const bool enabled) {
  for (auto &overlay : overlays) {
    overlay->set_capture_exclusion(enabled);
  }
}

void DaemonApp::Session::clear_input_state() {
  input.clear();
  for (auto &key : global_keyboard) {
    key.pressed = false;
  }
}

int DaemonApp::run(const HINSTANCE instance, const int show_command) {
  (void)show_command;
  instance_ = instance;
  if (!create_message_window(instance)) {
    return 1;
  }
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  reload_event_ = CreateEventW(nullptr, TRUE, FALSE, profile_reload_event_name);
  settings_reload_event_ = CreateEventW(
      nullptr, TRUE, FALSE, layerc::platform::settings_reload_event_name);
  register_input_devices();
  add_tray_icon();
  reload_profiles();
  foreground_hook_target = this;
  foreground_event_hook_ = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &DaemonApp::foreground_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  // Include destroy notifications so a closed matched game immediately retires
  // its instance and owned overlays rather than waiting for the next scan.
  location_event_hook_ = SetWinEventHook(
      EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
      &DaemonApp::foreground_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetTimer(window_, game_scan_timer, 1000, nullptr);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (foreground_hook_target == this) {
    foreground_hook_target = nullptr;
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
  remove_tray_icon();
  return static_cast<int>(message.wParam);
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
  window_ = CreateWindowExW(0, definition.lpszClassName, L"LayerD", 0, 0, 0,
                            0, 0, HWND_MESSAGE, nullptr, instance, this);
  return window_ != nullptr;
}

bool DaemonApp::register_input_devices() {
  const RAWINPUTDEVICE devices[] = {
      {.usUsagePage = 0x01,
       .usUsage = 0x06,
       .dwFlags = RIDEV_INPUTSINK,
       .hwndTarget = window_},
      {.usUsagePage = 0x01,
       .usUsage = 0x02,
       .dwFlags = RIDEV_INPUTSINK,
       .hwndTarget = window_},
  };
  return RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                                 sizeof(RAWINPUTDEVICE)) != FALSE;
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

std::filesystem::path DaemonApp::profile_directory() const {
  std::array<wchar_t, 32'768> override_path{};
  const DWORD override_length = GetEnvironmentVariableW(
      L"LAYERC_PROFILE_DIR", override_path.data(),
      static_cast<DWORD>(override_path.size()));
  if (override_length > 0 && override_length < override_path.size()) {
    return std::filesystem::path(
        std::wstring_view(override_path.data(), override_length));
  }
  PWSTR local_app_data{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                  nullptr, &local_app_data))) {
    return {};
  }
  std::filesystem::path result(local_app_data);
  CoTaskMemFree(local_app_data);
  return result / L"LayerC" / L"profiles";
}

void DaemonApp::reload_profiles() {
  std::vector<ProfileSource> loaded;
  const auto directory = profile_directory();
  std::error_code error;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, error);
  }
  if (!error && !directory.empty()) {
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() ||
          lower_copy(entry.path().extension().wstring()) != L".json") {
        continue;
      }
      const auto contents = read_profile_file(entry.path());
      if (!contents.has_value()) {
        continue;
      }
      auto parsed = layerc::core::parse_profile_json(*contents);
      if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed)) {
        loaded.push_back({.key = entry.path().lexically_normal().wstring(),
                          .profile = std::move(*profile)});
      }
    }
  }

  profile_sources_ = std::move(loaded);
  active_input_sessions_.clear();
  sessions_.clear();
  runtime_instances_.clear();
  for (const auto &source : profile_sources_) {
    if (source.profile.enabled && source.profile.game.global) {
      sessions_.push_back(std::make_unique<Session>(
          source.profile, source.key, capture_exclusion_));
    }
  }

  const bool has_enabled_profile = std::any_of(
      profile_sources_.begin(), profile_sources_.end(),
      [](const ProfileSource &source) { return source.profile.enabled; });
  update_tooltip(has_enabled_profile ? L"LayerD - profiles loaded"
                                     : L"LayerD - no enabled profiles");
  refresh_game_windows();
}

bool DaemonApp::reload_settings_if_requested() {
  if (settings_reload_event_ == nullptr ||
      WaitForSingleObject(settings_reload_event_, 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(settings_reload_event_);
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  for (auto &session : sessions_) {
    session->set_capture_exclusion(capture_exclusion_);
  }
  update_overlays();
  return true;
}

bool DaemonApp::reload_profiles_if_requested() {
  if (reload_event_ == nullptr ||
      WaitForSingleObject(reload_event_, 0) != WAIT_OBJECT_0) {
    return false;
  }
  ResetEvent(reload_event_);
  reload_profiles();
  return true;
}

void DaemonApp::refresh_game_windows() {
  using RuntimeKeys = std::vector<layerc::core::RuntimeInstanceKey>;
  RuntimeKeys desired;
  std::pair<DaemonApp *, RuntimeKeys *> context{this, &desired};
  EnumWindows(
      [](const HWND candidate, const LPARAM parameter) -> BOOL {
        auto &context = *reinterpret_cast<std::pair<DaemonApp *, RuntimeKeys *> *>(
            parameter);
        if (visible_area(candidate) == 0) {
          return TRUE;
        }
        for (const auto &source : context.first->profile_sources_) {
          if (!source.profile.enabled || source.profile.game.global ||
              !matches_profile(candidate, source.profile)) {
            continue;
          }
          context.second->push_back(
              {.profile_key = source.key,
               .window = reinterpret_cast<std::uintptr_t>(candidate)});
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  const auto changes = runtime_instances_.reconcile(std::move(desired));
  std::erase_if(active_input_sessions_, [&](const Session *session) {
    return !session->is_global() &&
           !runtime_instances_.contains(
               {.profile_key = session->source_key,
                .window = reinterpret_cast<std::uintptr_t>(session->game_window)});
  });
  std::erase_if(sessions_, [&](const std::unique_ptr<Session> &session) {
    if (session->is_global()) {
      return false;
    }
    return !runtime_instances_.contains(
        {.profile_key = session->source_key,
         .window = reinterpret_cast<std::uintptr_t>(session->game_window)});
  });

  for (const auto &key : changes.added) {
    const auto source = std::find_if(
        profile_sources_.begin(), profile_sources_.end(), [&](const ProfileSource &candidate) {
          return candidate.key == key.profile_key;
        });
    const auto window = reinterpret_cast<HWND>(key.window);
    if (source == profile_sources_.end() || !IsWindow(window)) {
      continue;
    }
    auto instance = std::make_unique<Session>(source->profile, source->key,
                                               capture_exclusion_);
    instance->game_window = window;
    sessions_.push_back(std::move(instance));
  }

  sync_foreground_state();
}
void DaemonApp::sync_foreground_state() {
  auto active_sessions = foreground_sessions();
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const HWND active_window =
      active_sessions.empty()
          ? nullptr
          : (active_sessions.front()->is_global()
                 ? foreground_root
                 : active_sessions.front()->game_window);
  const bool used_controller = std::any_of(
      active_input_sessions_.begin(), active_input_sessions_.end(),
      [](const Session *session) { return session->uses_controller; });
  const bool uses_controller = std::any_of(
      active_sessions.begin(), active_sessions.end(),
      [](const Session *session) { return session->uses_controller; });
  if (active_window != input_foreground_) {
    for (auto &session : sessions_) {
      session->clear_input_state();
    }
    input_foreground_ = active_window;
    if (uses_controller) {
      seed_controller_states();
    } else {
      controller_states_ = {};
    }
  } else {
    for (auto *previous : active_input_sessions_) {
      if (std::find(active_sessions.begin(), active_sessions.end(), previous) ==
          active_sessions.end()) {
        previous->clear_input_state();
      }
    }
    if (uses_controller && !used_controller) {
      seed_controller_states();
    } else if (!uses_controller) {
      controller_states_ = {};
    }
  }
  active_input_sessions_ = std::move(active_sessions);

  const bool needs_input_polling = std::any_of(
      active_input_sessions_.begin(), active_input_sessions_.end(),
      [](const Session *session) { return session->requires_input_polling(); });
  if (needs_input_polling && !input_timer_running_) {
    SetTimer(window_, input_poll_timer, 16, nullptr);
    input_timer_running_ = true;
  } else if (!needs_input_polling && input_timer_running_) {
    KillTimer(window_, input_poll_timer);
    input_timer_running_ = false;
    controller_states_ = {};
    for (auto &session : sessions_) {
      session->clear_input_state();
    }
  }
  update_overlays();
}

void CALLBACK DaemonApp::foreground_event_proc(
    const HWINEVENTHOOK hook, const DWORD event, const HWND window,
    const LONG object_id, const LONG child_id, const DWORD event_thread,
    const DWORD event_time) {
  (void)hook;
  (void)child_id;
  (void)event_thread;
  (void)event_time;
  if (foreground_hook_target == nullptr) {
    return;
  }
  if (event == EVENT_SYSTEM_FOREGROUND) {
    foreground_hook_target->sync_foreground_state();
  } else if (event == EVENT_OBJECT_DESTROY && object_id == OBJID_WINDOW) {
    foreground_hook_target->refresh_game_windows();
  } else if (event == EVENT_OBJECT_LOCATIONCHANGE && object_id == OBJID_WINDOW) {
    foreground_hook_target->sync_overlay_location(window);
  }
}

void DaemonApp::sync_overlay_location(const HWND changed_window) {
  const HWND changed_root = GetAncestor(changed_window, GA_ROOT);
  if (changed_root == nullptr) {
    return;
  }
  if (IsIconic(changed_root) || !IsWindowVisible(changed_root)) {
    refresh_game_windows();
    return;
  }
  const HWND foreground_root = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const bool affects_runtime = std::any_of(
      sessions_.begin(), sessions_.end(), [&](const std::unique_ptr<Session> &session) {
        return (!session->is_global() && session->game_window == changed_root) ||
               (session->is_global() && changed_root == foreground_root);
      });
  if (affects_runtime) {
    update_overlays();
  }
}

std::vector<DaemonApp::Session *> DaemonApp::foreground_sessions() {
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  const auto active = runtime_instances_.foreground(
      reinterpret_cast<std::uintptr_t>(foreground));
  std::vector<Session *> matches;
  matches.reserve(active.size());
  for (auto &session : sessions_) {
    if (session->is_global()) continue;
    const auto found = std::find_if(
        active.begin(), active.end(),
        [&](const layerc::core::RuntimeInstanceKey &key) {
          return session->source_key == key.profile_key &&
                 reinterpret_cast<std::uintptr_t>(session->game_window) ==
                     key.window;
        });
    if (found != active.end()) matches.push_back(session.get());
  }
  if (!matches.empty()) return matches;
  for (auto &session : sessions_) {
    if (session->is_global()) matches.push_back(session.get());
  }
  return matches;
}
RECT DaemonApp::overlay_target() const {
  RECT target{};
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  if (foreground != nullptr && foreground != window_ && !IsIconic(foreground)) {
    RECT client{};
    POINT origin{};
    if (GetClientRect(foreground, &client) && ClientToScreen(foreground, &origin)) {
      target = {origin.x, origin.y, origin.x + client.right - client.left,
                origin.y + client.bottom - client.top};
      if (target.right > target.left && target.bottom > target.top) {
        return target;
      }
    }
  }
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0) &&
      target.right > target.left && target.bottom > target.top) {
    return target;
  }
  return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void DaemonApp::handle_raw_input(const HRAWINPUT handle) {
  UINT size{};
  if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
      size == 0) {
    return;
  }
  std::array<std::byte, sizeof(RAWINPUT)> stack_buffer{};
  std::vector<std::byte> heap_buffer;
  std::byte *buffer = stack_buffer.data();
  if (size > stack_buffer.size()) {
    heap_buffer.resize(size);
    buffer = heap_buffer.data();
  }
  if (GetRawInputData(handle, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) !=
      size) {
    return;
  }

  const auto &input = *reinterpret_cast<const RAWINPUT *>(buffer);
  if (input.header.dwType == RIM_TYPEKEYBOARD) {
    const auto &keyboard = input.data.keyboard;
    std::uint16_t code = keyboard.MakeCode;
    if ((keyboard.Flags & RI_KEY_E0) != 0) {
      code = static_cast<std::uint16_t>(code | 0xE000U);
    }
    emit_input({.token = {.device = layerc::core::InputDevice::keyboard, .code = code},
                .pressed = (keyboard.Flags & RI_KEY_BREAK) == 0});
  } else if (input.header.dwType == RIM_TYPEMOUSE) {
    const auto flags = input.data.mouse.usButtonFlags;
    const auto emit_button = [this, flags](const USHORT down, const USHORT up,
                                           const std::uint16_t code) {
      if ((flags & down) != 0) {
        emit_input({{layerc::core::InputDevice::mouse, code, 0}, true});
      }
      if ((flags & up) != 0) {
        emit_input({{layerc::core::InputDevice::mouse, code, 0}, false});
      }
    };
    emit_button(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1);
    emit_button(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2);
    emit_button(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 3);
    emit_button(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4);
    emit_button(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5);
    if ((flags & RI_MOUSE_WHEEL) != 0) {
      const auto delta = static_cast<SHORT>(input.data.mouse.usButtonData);
      const std::uint16_t code = delta > 0 ? 6 : 7;
      emit_input({{layerc::core::InputDevice::mouse, code, 0}, true});
      emit_input({{layerc::core::InputDevice::mouse, code, 0}, false});
    }
  }
}

void DaemonApp::poll_input_devices() {
  if (active_input_sessions_.empty()) return;
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != input_foreground_) {
    sync_foreground_state();
    return;
  }
  bool changed{};
  for (auto *session : active_input_sessions_) {
    if (session->is_global()) {
      changed = poll_global_keyboard(*session) || changed;
    }
  }
  if (std::any_of(active_input_sessions_.begin(), active_input_sessions_.end(),
                  [](const Session *session) {
                    return session->uses_controller;
                  })) {
    poll_controllers();
  }
  if (changed) update_overlays();
}

bool DaemonApp::poll_global_keyboard(Session &session) {
  bool changed{};
  for (auto &key : session.global_keyboard) {
    const UINT scan = static_cast<UINT>(key.token.code);
    const UINT virtual_key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    if (virtual_key == 0) continue;
    const bool pressed =
        (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    const bool was_pressed = key.pressed;
    key.pressed = pressed;
    if (pressed != was_pressed) {
      changed =
          process_input(session, {.token = key.token, .pressed = pressed}) ||
          changed;
    }
  }
  return changed;
}

void DaemonApp::seed_controller_states() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    controller_states_[index] = state;
  }
}
void DaemonApp::poll_controllers() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    const WORD previous = controller_states_[index].Gamepad.wButtons;
    const WORD current = state.Gamepad.wButtons;
    for (unsigned bit = 0; bit < 16; ++bit) {
      const WORD mask = static_cast<WORD>(1U << bit);
      if ((previous & mask) != (current & mask)) {
        emit_input({.token = {.device = layerc::core::InputDevice::controller,
                              .code = static_cast<std::uint16_t>(bit + 1U),
                              .controller_index = static_cast<std::uint8_t>(index)},
                    .pressed = (current & mask) != 0});
      }
    }
    const auto emit_trigger = [this, index](const BYTE old_value,
                                            const BYTE new_value,
                                            const std::uint16_t code) {
      const bool old_pressed = old_value >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      const bool new_pressed = new_value >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      if (old_pressed != new_pressed) {
        emit_input({.token = {.device = layerc::core::InputDevice::controller,
                              .code = code,
                              .controller_index = static_cast<std::uint8_t>(index)},
                    .pressed = new_pressed});
      }
    };
    emit_trigger(controller_states_[index].Gamepad.bLeftTrigger,
                 state.Gamepad.bLeftTrigger, 17);
    emit_trigger(controller_states_[index].Gamepad.bRightTrigger,
                 state.Gamepad.bRightTrigger, 18);
    controller_states_[index] = state;
  }
}

void DaemonApp::emit_input(const layerc::core::InputEvent &event) {
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != input_foreground_)
    sync_foreground_state();
  if (active_input_sessions_.empty()) return;
  bool changed{};
  for (auto *session : active_input_sessions_)
    changed = process_input(*session, event) || changed;
  if (changed) update_overlays();
}

bool DaemonApp::process_input(Session &session,
                              const layerc::core::InputEvent &event) {
  const auto triggered = session.input.process(event);
  const auto now = layerc::core::MonotonicClock::now();
  bool changed{};
  for (const auto &cooldown_id : triggered) {
    if (session.cooldowns.trigger(cooldown_id, now)) {
      changed = true;
      update_tooltip(L"LayerD - triggered " + utf8_to_wide(cooldown_id));
    }
  }
  return changed;
}

void DaemonApp::update_overlays() {
  const auto now = layerc::core::MonotonicClock::now();
  bool has_running_cooldown{};

  for (auto &session : sessions_) {
    const bool attached_window_invalid =
        !session->is_global() &&
        (session->game_window == nullptr || !IsWindow(session->game_window) ||
         !IsWindowVisible(session->game_window) || IsIconic(session->game_window));
    const bool input_active =
        !session->is_global() ||
        std::find(active_input_sessions_.begin(), active_input_sessions_.end(),
                  session.get()) != active_input_sessions_.end();
    if (attached_window_invalid || !input_active) {
      for (auto &overlay : session->overlays) {
        overlay->hide();
      }
      continue;
    }

    session->cooldowns.snapshot(now, session->snapshot_buffer);
    const auto &states = session->snapshot_buffer;
    const bool session_is_running = std::any_of(
        states.begin(), states.end(), [](const layerc::core::CooldownState &state) {
          return state.next_charge_at.has_value();
        });
    has_running_cooldown = has_running_cooldown || session_is_running;

    for (auto &overlay : session->overlays) {
      if (!overlay->should_show(states)) {
        overlay->hide();
        continue;
      }
      if (session->is_global()) {
        overlay->show_on_rect(overlay_target());
      } else {
        // A background game's overlay remains owned by that game and is never
        // raised above the foreground application.
        overlay->show_for(session->game_window);
      }
      overlay->render(states, now);
    }
  }

  if (has_running_cooldown && !overlay_timer_running_) {
    SetTimer(window_, overlay_render_timer, 16, nullptr);
    overlay_timer_running_ = true;
  } else if (!has_running_cooldown && overlay_timer_running_) {
    KillTimer(window_, overlay_render_timer);
    overlay_timer_running_ = false;
  }
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
  auto *app = reinterpret_cast<DaemonApp *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
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
  case WM_TIMER:
    if (wparam == game_scan_timer) {
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
    KillTimer(window, game_scan_timer);
    KillTimer(window, input_poll_timer);
    KillTimer(window, overlay_render_timer);
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(window, message, wparam, lparam);
  }
}

} // namespace layerc::daemon