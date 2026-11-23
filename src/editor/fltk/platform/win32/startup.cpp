#include "startup.hpp"

#include "desktop_listener.hpp"
#include "editor_app.hpp"
#include "editor_style.hpp"
#include "settings_store.hpp"
#include "steady_timeout.hpp"
#include "window_chrome.hpp"
#include "window_settings.hpp"
#include "layerc/platform/editor_instance.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/win32_handle.hpp"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/platform.H>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace layerc::editor {
namespace {

constexpr char editor_name[] = "LayerE";

struct ComScope {
  ~ComScope() { CoUninitialize(); }
};

void install_editor_icon(const HINSTANCE instance) {
  const auto load = [instance](const int metric) {
    const int size = GetSystemMetrics(metric);
    return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, size, size, LR_SHARED));
  };
  Fl_Window::default_icons(load(SM_CXICON), load(SM_CXSMICON));
}

struct StartupSettings {
  StartupAppearance appearance;
  bool capture_exclusion_read_failed{};
};

StartupSettings load_startup_settings() {
  auto appearance = load_startup_appearance();
  load_startup_text_scale();
  const auto capture = platform::read_bool_setting(platform::BoolSetting::editor_capture_exclusion);
  set_editor_capture_exclusion(capture.value.value_or(false));
  return {std::move(appearance), capture.failed()};
}

void fit_initial_window(const HWND window) {
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  RECT work{};
  if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                      &monitor)) {
    work = monitor.rcWork;
  } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    return;
  }
  const int dpi = static_cast<int>(platform::native_window_dpi(window));
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int width = std::clamp(MulDiv(1210, dpi, USER_DEFAULT_SCREEN_DPI),
                               1, std::max(1, work_width));
  const int height = std::clamp(MulDiv(760, dpi, USER_DEFAULT_SCREEN_DPI),
                                1, std::max(1, work_height));
  const int x = static_cast<int>(work.left) + std::max(0, (work_width - width) / 2);
  const int y = static_cast<int>(work.top) + std::max(0, (work_height - height) / 2);
  SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool smoke_store_overridden() {
  const auto profile = GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", nullptr, 0);
  const auto settings = GetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", nullptr, 0);
  return profile > 1 && profile <= 32'768 && settings > 1 && settings <= 256;
}

} // namespace

std::optional<std::string>
recover_startup_profiles(const std::filesystem::path &directory) {
  const auto recovered = platform::recover_profile_transactions(directory);
  if (!recovered.failure) return std::nullopt;
  return std::string(editor_name) + " could not recover an interrupted profile import (" +
         platform::profile_write_error_text(*recovered.failure) +
         "). Resolve the _transactions directory before editing profiles.";
}

int run_windows_editor(const HINSTANCE instance, const std::wstring_view command_line) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    return 1;
  const ComScope com;
  Fl::keyboard_screen_scaling(0);
  fl_open_display();
  Fl_Window::default_xclass(platform::editor_fltk_window_class);
  install_editor_icon(instance);

  const bool smoke = command_line == L"--smoke";
  const platform::SingleInstanceGuard guard(platform::editor_single_instance_name().c_str());
  if (guard.state() == platform::SingleInstanceGuard::State::failed) {
    fl_message_title(editor_name);
    fl_alert("%s could not create its single-instance guard.", editor_name);
    return 1;
  }
  if (guard.state() == platform::SingleInstanceGuard::State::already_running) {
    platform::raise_existing_editor();
    if (smoke) std::fputs("Smoke requires the editor single-instance guard.\n", stderr);
    return smoke ? 1 : 0;
  }

  if (smoke && !smoke_store_overridden()) {
    std::fputs("Smoke requires LAYERC_PROFILE_DIR and LAYERC_SETTINGS_KEY overrides.\n", stderr);
    return 1;
  }
  if (const auto error = recover_startup_profiles(platform::profile_directory())) {
    fl_message_title("LayerC profile recovery");
    fl_alert("%s", error->c_str());
    return 1;
  }

  auto settings = load_startup_settings();
  install_editor_style();
  EditorApp app;
  WindowsWindowDecorator decorator;
  app.build(std::move(settings.appearance.editor));
  decorator.set_warning_reporter([&app](std::string_view warning) {
    app.set_capture_exclusion_warning(warning);
  });
  const WindowsTextScaleShortcuts text_shortcuts([&app](StatusKind kind, std::string text) {
    app.report(kind, std::move(text));
  });
  const WindowsDesktopListener desktop(instance, [&app] { app.refresh_desktop_appearance(); });
  if (!desktop.native_handle())
    std::fputs("The LayerE desktop change listener could not be created.\n", stderr);
  app.show();
  auto *const window = app.main_window();
  const HWND handle = fl_xid(window);
  if (handle == nullptr) return 1;
  fit_initial_window(handle);
  decorator.refresh();
  if (!app.sync_accessibility()) return 1;
  if (settings.capture_exclusion_read_failed)
    app.report(StatusKind::warning, "Could not read the editor capture-exclusion setting.");

  SteadyTimeout smoke_close([window] { window->do_callback(); });
  if (smoke) smoke_close.start(std::chrono::milliseconds(200));
  return Fl::run();
}

} // namespace layerc::editor
