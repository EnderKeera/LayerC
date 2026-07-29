#include "editor_app.hpp"

#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/windows.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::editor {
namespace {

constexpr int dark_title_bar_attribute = 20;
constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";

constexpr wchar_t owner_draw_checkbox_property[] =
    L"LayerC.OwnerDrawCheckboxChecked";

void set_owner_draw_checkbox(const HWND control, const bool checked) {
  if (control == nullptr) return;
  if (checked) {
    SetPropW(control, owner_draw_checkbox_property,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
  } else {
    RemovePropW(control, owner_draw_checkbox_property);
  }
  InvalidateRect(control, nullptr, TRUE);
}

[[nodiscard]] bool owner_draw_checkbox_checked(const HWND control) {
  return control != nullptr &&
         GetPropW(control, owner_draw_checkbox_property) != nullptr;
}

void toggle_owner_draw_checkbox(const HWND control) {
  set_owner_draw_checkbox(control, !owner_draw_checkbox_checked(control));
}

struct RoleColors {
  COLORREF fill{};
  COLORREF text{};
  COLORREF border{};
  COLORREF hover{};
  COLORREF selection{};
  COLORREF disabled{};
};

struct ThemeBase {
  std::uint32_t background{};
  std::uint32_t panel{};
  std::uint32_t input{};
  std::uint32_t border{};
  std::uint32_t text{};
  std::uint32_t muted{};
  std::uint32_t accent{};
};

constexpr ThemeBase copper{0x1B1F24, 0x282D34, 0x20252B, 0x4B525C,
                           0xE9E4DD, 0x9CA3AD, 0xD88E4B};
constexpr ThemeBase teal{0x182021, 0x263436, 0x1F292B, 0x466367,
                         0xE1ECEA, 0x96AAA8, 0x5AB6AA};
constexpr ThemeBase plum{0x211E29, 0x312B3C, 0x282231, 0x5A506B,
                         0xECE7F1, 0xB0A7BC, 0xB382E2};

COLORREF color_ref(const std::uint32_t color) {
  return RGB((color >> 16U) & 0xFFU, (color >> 8U) & 0xFFU,
             color & 0xFFU);
}

std::uint32_t rgb_value(const COLORREF color) {
  return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
         (static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
         static_cast<std::uint32_t>(GetBValue(color));
}

const ThemeBase &theme_base(const layerc::platform::EditorTheme theme) {
  switch (theme) {
  case layerc::platform::EditorTheme::teal:
    return teal;
  case layerc::platform::EditorTheme::plum:
    return plum;
  case layerc::platform::EditorTheme::copper:
  default:
    return copper;
  }
}

RoleColors default_colors(const layerc::platform::EditorTheme theme,
                          const std::uint32_t accent,
                          const layerc::platform::EditorControlRole role) {
  const auto &base = theme_base(theme);
  const auto accent_color = accent == 0 ? base.accent : accent;
  const auto accent_dark =
      ((accent_color & 0xFF0000U) * 55U / 100U & 0xFF0000U) |
      ((accent_color & 0x00FF00U) * 55U / 100U & 0x00FF00U) |
      ((accent_color & 0x0000FFU) * 55U / 100U & 0x0000FFU);
  RoleColors colors{.fill = color_ref(base.background),
                    .text = color_ref(base.text),
                    .border = color_ref(base.border),
                    .hover = color_ref(base.panel),
                    .selection = color_ref(accent_dark),
                    .disabled = color_ref(base.muted)};
  switch (role) {
  case layerc::platform::EditorControlRole::toolbar:
    colors.fill = color_ref(base.background);
    break;
  case layerc::platform::EditorControlRole::sidebar:
    colors.fill = color_ref(base.panel);
    break;
  case layerc::platform::EditorControlRole::label:
  case layerc::platform::EditorControlRole::status:
    colors.text = color_ref(base.muted);
    break;
  case layerc::platform::EditorControlRole::button:
  case layerc::platform::EditorControlRole::checkbox:
    colors.fill = color_ref(base.panel);
    colors.hover = color_ref(base.input);
    break;
  case layerc::platform::EditorControlRole::edit:
  case layerc::platform::EditorControlRole::list:
  case layerc::platform::EditorControlRole::preview:
    colors.fill = color_ref(base.input);
    break;
  case layerc::platform::EditorControlRole::window:
  case layerc::platform::EditorControlRole::count:
    break;
  }
  return colors;
}

RoleColors resolved_colors(const layerc::platform::EditorTheme theme,
                           const std::uint32_t accent,
                           const layerc::platform::EditorControlRole role) {
  auto colors = default_colors(theme, accent, role);
  const auto overrides = layerc::platform::editor_appearance_overrides(role);
  if ((overrides.fields & layerc::platform::editor_fill) != 0) colors.fill = color_ref(overrides.fill);
  if ((overrides.fields & layerc::platform::editor_text) != 0) colors.text = color_ref(overrides.text);
  if ((overrides.fields & layerc::platform::editor_border) != 0) colors.border = color_ref(overrides.border);
  if ((overrides.fields & layerc::platform::editor_hover) != 0) colors.hover = color_ref(overrides.hover);
  if ((overrides.fields & layerc::platform::editor_selection) != 0) colors.selection = color_ref(overrides.selection);
  if ((overrides.fields & layerc::platform::editor_disabled) != 0) colors.disabled = color_ref(overrides.disabled);
  return colors;
}

std::wstring to_wide(const std::string_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0) return L"?";
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count) == 0) return L"?";
  return output;
}

std::string to_utf8(const std::wstring_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) return {};
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) return {};
  std::string output(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), output.data(), count, nullptr, nullptr) == 0) return {};
  return output;
}

std::wstring control_text(const HWND control) {
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};
  std::wstring output(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied = GetWindowTextW(control, output.data(), static_cast<int>(output.size()));
  output.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return output;
}

void set_text(const HWND control, const std::wstring_view text) {
  if (control != nullptr) SetWindowTextW(control, std::wstring(text).c_str());
}

enum class PreferredAppMode : int {
  default_mode,
  allow_dark,
  force_dark,
  force_light,
  maximum,
};

HMODULE uxtheme_module() {
  static const HMODULE module = [] {
    HMODULE loaded = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return loaded != nullptr ? loaded : LoadLibraryW(L"uxtheme.dll");
  }();
  return module;
}

void enable_native_dark_mode() {
  const HMODULE module = uxtheme_module();
  if (module == nullptr) return;
  using SetPreferredAppMode = PreferredAppMode(WINAPI *)(PreferredAppMode);
  using FlushMenuThemes = void(WINAPI *)();
  if (const auto set_mode = reinterpret_cast<SetPreferredAppMode>(
          GetProcAddress(module, MAKEINTRESOURCEA(135)));
      set_mode != nullptr)
    (void)set_mode(PreferredAppMode::allow_dark);
  if (const auto flush = reinterpret_cast<FlushMenuThemes>(
          GetProcAddress(module, MAKEINTRESOURCEA(136)));
      flush != nullptr)
    flush();
}

void set_dark_title_bar(const HWND window) {
  if (const HMODULE module = uxtheme_module(); module != nullptr) {
    using AllowDarkModeForWindow = BOOL(WINAPI *)(HWND, BOOL);
    if (const auto allow = reinterpret_cast<AllowDarkModeForWindow>(
            GetProcAddress(module, MAKEINTRESOURCEA(133)));
        allow != nullptr)
      (void)allow(window, TRUE);
  }
  SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
  const BOOL enabled = TRUE;
  (void)DwmSetWindowAttribute(window, dark_title_bar_attribute, &enabled, sizeof(enabled));
}

std::filesystem::path profile_directory() {
  std::array<wchar_t, 32'768> override_path{};
  const DWORD length = GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", override_path.data(),
                                                static_cast<DWORD>(override_path.size()));
  if (length > 0 && length < override_path.size())
    return std::filesystem::path(std::wstring_view(override_path.data(), length));
  PWSTR local_app_data{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                  &local_app_data))) return {};
  std::filesystem::path directory(local_app_data);
  CoTaskMemFree(local_app_data);
  return directory / L"LayerC" / L"profiles";
}

std::optional<std::string> read_profile_file(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > layerc::core::max_profile_json_bytes) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  return input.gcount() == static_cast<std::streamsize>(contents.size())
             ? std::optional<std::string>(std::move(contents)) : std::nullopt;
}

bool write_profile_file(const std::filesystem::path &path, const std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  const auto temporary = path.wstring() + L".tmp";
  {
    std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) return false;
  }
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool signal_profile_reload() {
  const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, profile_reload_event_name);
  if (event == nullptr) return false;
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

bool start_daemon() {
  std::array<wchar_t, 32'768> module_path{};
  const DWORD length = GetModuleFileNameW(nullptr, module_path.data(),
                                           static_cast<DWORD>(module_path.size()));
  if (length == 0 || length >= module_path.size()) return false;
  const auto daemon = std::filesystem::path(std::wstring_view(module_path.data(), length)).parent_path() /
                      L"layerd.exe";
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", daemon.c_str(), nullptr,
                                                   daemon.parent_path().c_str(), SW_SHOWNOACTIVATE)) > 32;
}

bool recycle_profile_file(const std::filesystem::path &path) {
  std::wstring source = path.wstring();
  source.push_back(L'\0');
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = source.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
}

std::string safe_identifier(const std::wstring_view text, const std::string_view fallback = "profile") {
  std::string output;
  bool last_dash{};
  for (const wchar_t character : text) {
    const wchar_t lower = static_cast<wchar_t>(std::towlower(character));
    if ((lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9')) {
      output.push_back(static_cast<char>(lower)); last_dash = false;
    } else if (!output.empty() && !last_dash) { output.push_back('-'); last_dash = true; }
  }
  while (!output.empty() && output.back() == '-') output.pop_back();
  return output.empty() ? std::string(fallback) : output;
}

layerc::core::GameProfile make_new_profile() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  GameProfile profile;
  profile.id = "new-profile";
  profile.name = "New Profile";
  profile.enabled = true;
  profile.cooldowns.push_back({.id = "ability", .name = "Ability", .duration = 5s,
                               .max_charges = 1, .retrigger = RetriggerPolicy::ignore});
  profile.panels.push_back({.id = "main", .name = "Main", .style = PanelStyle::bar,
                            .anchor_x = 0.5, .anchor_y = 0.75, .cooldown_ids = {"ability"}});
  profile.bindings.push_back({.cooldown_id = "ability",
                              .chord = InputChord{{{InputDevice::keyboard, 0x12, 0}}}});
  return profile;
}

std::wstring profile_list_label(const layerc::core::GameProfile &profile) {
  std::wstring label = to_wide(profile.name);
  if (!profile.game.executable_name.empty())
    label += L"  —  " + to_wide(profile.game.executable_name);
  if (!profile.enabled) label += L"  (disabled)";
  return label;
}

std::string unique_panel_id(
    const std::string_view base,
    const std::vector<layerc::core::Panel> &existing,
    const std::vector<layerc::core::Panel> *generated = nullptr) {
  const auto is_used = [&](const std::string &candidate) {
    const auto contains = [&](const std::vector<layerc::core::Panel> &panels) {
      return std::any_of(panels.begin(), panels.end(),
                         [&](const layerc::core::Panel &panel) {
                           return panel.id == candidate;
                         });
    };
    return contains(existing) || (generated != nullptr && contains(*generated));
  };
  std::string candidate(base);
  for (std::size_t suffix = 2; is_used(candidate); ++suffix)
    candidate = std::string(base) + "-" + std::to_string(suffix);
  return candidate;
}

std::wstring profile_error_text(const layerc::core::ProfileError &error) {
  const auto path = to_wide(error.path);
  const auto message = to_wide(error.message);
  return path.empty() ? message : path + L": " + message;
}

std::uint32_t parse_uint(const std::wstring_view text, const std::uint32_t fallback) {
  const std::wstring value(text);
  wchar_t *end{};
  const auto parsed = wcstoul(value.c_str(), &end, 10);
  return end != nullptr && end != value.c_str() && *end == L'\0' && parsed <= UINT32_MAX
             ? static_cast<std::uint32_t>(parsed) : fallback;
}

std::wstring color_label(const wchar_t *label, const std::uint32_t value) {
  wchar_t text[64]{};
  swprintf_s(text, L"%ls  #%06X", label, value & 0x00FFFFFFU);
  return text;
}

} // namespace

class EditorWindow {
public:
  int run(HINSTANCE instance, int show_command);

private:
  struct ProfileEntry { std::filesystem::path path; std::wstring label; bool enabled{true}; };

  static INT_PTR CALLBACK dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK task_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK options_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK appearance_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK preview_subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  bool handle_binding_capture_message(const MSG &message);

  [[nodiscard]] RoleColors colors(layerc::platform::EditorControlRole role) const {
    return resolved_colors(theme_, accent_, role);
  }
  void initialize(HWND);
  void initialize_tasks(HWND);
  void initialize_options(HWND);
  void initialize_appearance(HWND);
  void style_dialog_children(HWND) const;
  void style_list_view(HWND, bool hide_header) const;
  void rebuild_profile_check_images();
  void rebuild_brushes();
  void refresh_theme();
  void layout();
  void set_status(std::wstring_view text) { set_text(status_, text); }
  [[nodiscard]] HBRUSH brush(layerc::platform::EditorControlRole role) const {
    return brushes_[static_cast<std::size_t>(role)];
  }

  void refresh_profile_controls();
  void refresh_cooldown_list(int selection = 0);
  [[nodiscard]] layerc::core::CooldownDefinition *selected_cooldown();
  void refresh_binding_controls();
  void begin_binding_capture();
  void cancel_binding_capture(std::wstring_view status = {});
  void assign_binding_key(LPARAM key_data);
  void refresh_bar_list(std::size_t preferred_panel = SIZE_MAX);
  void refresh_bar_controls();
  void sync_profile_from_controls();
  [[nodiscard]] layerc::core::Panel *selected_bar();
  [[nodiscard]] const layerc::core::Panel *selected_bar() const;
  [[nodiscard]] bool validate_profile();
  [[nodiscard]] bool confirm_unsaved_changes();
  void new_profile(bool confirm = true);
  void reload_profile_list(const std::filesystem::path &select_path = {}, bool open_selection = true);
  void open_profile_row(int row, bool confirm);
  void save_profile_enabled(int row, bool enabled);
  [[nodiscard]] bool choose_save_path(std::filesystem::path &path);
  [[nodiscard]] bool save_profile(bool force_save_as);
  void delete_profile();
  void open_profiles_folder();
  void show_file_menu();
  void attach_profile();

  void refresh_tasks();
  [[nodiscard]] HWND selected_task_window() const;
  [[nodiscard]] const layerc::platform::WindowCandidate *selected_task() const;
  void update_task_detail();
  void apply_task(const layerc::platform::WindowCandidate &candidate);
  void use_selected_task();
  void show_tasks();
  void begin_picker();
  void finish_picker();
  void cancel_picker();

  void add_cooldown();
  void remove_cooldown();
  void split_independent_bars();
  void choose_bar_color(int id);

  [[nodiscard]] double preview_aspect_ratio() const;
  [[nodiscard]] RECT preview_canvas() const;
  [[nodiscard]] RECT bar_preview_rect(const layerc::core::Panel &, const RECT &) const;
  void paint_preview(HWND);
  bool handle_preview_message(HWND, UINT, WPARAM, LPARAM);
  void update_dragged_bar(POINT);

  void open_options();
  void apply_options(HWND);
  [[nodiscard]] layerc::platform::EditorControlRole appearance_role(HWND) const;
  static std::uint32_t *appearance_value(layerc::platform::EditorAppearanceOverrides &, std::uint32_t);
  [[nodiscard]] COLORREF appearance_color(HWND, std::uint32_t) const;
  void refresh_appearance_controls(HWND);
  void choose_appearance_color(HWND, std::uint32_t);
  void reset_appearance(HWND);

  void draw_button(const DRAWITEMSTRUCT &, layerc::platform::EditorControlRole,
                   std::optional<COLORREF> fill = std::nullopt) const;
  void draw_list_box_item(const DRAWITEMSTRUCT &) const;
  void draw_checkbox(const DRAWITEMSTRUCT &) const;
  void draw_appearance_preview(const DRAWITEMSTRUCT &, HWND) const;
  LRESULT color_message(UINT, WPARAM, LPARAM) const;
  LRESULT handle_main_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_task_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_options_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_appearance_message(HWND, UINT, WPARAM, LPARAM);

  HINSTANCE instance_{};
  HWND dialog_{};
  HWND task_dialog_{};
  HWND profile_list_{};
  HWND cooldown_list_{};
  HWND bar_list_{};
  HWND preview_{};
  HWND status_{};
  HWND application_path_{};
  HWND task_list_{};
  HIMAGELIST profile_check_images_{};
  HFONT font_{};
  std::array<HBRUSH, static_cast<std::size_t>(layerc::platform::EditorControlRole::count)> brushes_{};
  std::array<COLORREF, 16> custom_colors_{};
  std::vector<ProfileEntry> entries_;
  std::vector<layerc::platform::WindowCandidate> task_candidates_;
  layerc::core::GameProfile profile_;
  std::filesystem::path current_path_;
  layerc::platform::EditorTheme theme_{layerc::platform::EditorTheme::copper};
  std::uint32_t accent_{0xD88E4BU};
  bool capture_exclusion_{true};
  bool dirty_{};
  bool syncing_{};
  bool picking_{};
  bool task_was_visible_{};
  bool dragging_bar_{};
  bool bar_drag_moved_{};
  POINT bar_drag_start_{};
  POINT bar_drag_offset_{};
  bool capturing_binding_{};
  std::size_t bar_controls_panel_{SIZE_MAX};
};
int EditorWindow::run(const HINSTANCE instance, const int show_command) {
  instance_ = instance;
  enable_native_dark_mode();
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_LISTVIEW_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  dialog_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_EDITOR), nullptr,
                               &EditorWindow::dialog_proc,
                               reinterpret_cast<LPARAM>(this));
  if (dialog_ == nullptr) return 1;
  // CW_USEDEFAULT only has meaning when a window is created.  Passing it to
  // SetWindowPos placed the dialog at (-2147483648, -2147483648), leaving a
  // running but invisible editor.  Center it inside the primary work area
  // instead, while keeping the compact fixed initial size.
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int work_width = static_cast<int>(work.right - work.left);
  const int work_height = static_cast<int>(work.bottom - work.top);
  const int width = std::min(1210, std::max(640, work_width));
  const int height = std::min(760, std::max(480, work_height));
  const int left = static_cast<int>(work.left) + std::max(0, (work_width - width) / 2);
  const int top = static_cast<int>(work.top) + std::max(0, (work_height - height) / 2);
  SetWindowPos(dialog_, nullptr, left, top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(dialog_, show_command);
  UpdateWindow(dialog_);
  const HACCEL accelerators =
      LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_EDITOR_ACCELERATORS));
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (handle_binding_capture_message(message)) continue;
    if (accelerators != nullptr &&
        TranslateAcceleratorW(dialog_, accelerators, &message) != 0) continue;
    const bool main_handled = IsDialogMessageW(dialog_, &message) != FALSE;
    const bool task_handled = task_dialog_ != nullptr && IsWindow(task_dialog_) &&
                              IsDialogMessageW(task_dialog_, &message) != FALSE;
    if (!main_handled && !task_handled) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}

INT_PTR CALLBACK EditorWindow::dialog_proc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_main_message(window, message, wparam, lparam);
}

INT_PTR CALLBACK EditorWindow::task_dialog_proc(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize_tasks(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_task_message(window, message, wparam, lparam);
}

INT_PTR CALLBACK EditorWindow::options_dialog_proc(HWND window, UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize_options(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_options_message(window, message, wparam, lparam);
}

INT_PTR CALLBACK EditorWindow::appearance_dialog_proc(HWND window, UINT message, WPARAM wparam,
                                                      LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize_appearance(window);
    return TRUE;
  }
  return editor == nullptr ? FALSE : editor->handle_appearance_message(window, message, wparam, lparam);
}

LRESULT CALLBACK EditorWindow::preview_subclass(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR subclass_id,
                                                DWORD_PTR reference) {
  auto *editor = reinterpret_cast<EditorWindow *>(reference);
  if (editor != nullptr && editor->handle_preview_message(window, message, wparam, lparam)) return 0;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &EditorWindow::preview_subclass, subclass_id);
  return DefSubclassProc(window, message, wparam, lparam);
}
bool EditorWindow::handle_binding_capture_message(const MSG &message) {
  if (!capturing_binding_) return false;
  if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
    if (message.wParam == VK_ESCAPE) {
      cancel_binding_capture(L"Binding capture cancelled.");
    } else {
      assign_binding_key(message.lParam);
    }
    return true;
  }
  if ((message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
       message.message == WM_MBUTTONDOWN) &&
      message.hwnd != GetDlgItem(dialog_, IDC_CAPTURE_BINDING)) {
    cancel_binding_capture();
  }
  return false;
}

void EditorWindow::initialize(const HWND window) {
  dialog_ = window;
  const UINT window_dpi = GetDpiForWindow(window);
  const int font_height = -MulDiv(9, window_dpi == 0 ? 96 : static_cast<int>(window_dpi), 72);
  font_ = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  theme_ = layerc::platform::editor_theme();
  accent_ = layerc::platform::editor_accent();
  capture_exclusion_ = layerc::platform::capture_exclusion_enabled();
  set_dark_title_bar(dialog_);
  (void)layerc::platform::apply_capture_exclusion(dialog_, capture_exclusion_);
  style_dialog_children(dialog_);
  rebuild_brushes();
  profile_list_ = GetDlgItem(dialog_, IDC_PROFILE_LIST);
  cooldown_list_ = GetDlgItem(dialog_, IDC_COOLDOWN_LIST);
  bar_list_ = GetDlgItem(dialog_, IDC_BAR_LIST);
  preview_ = GetDlgItem(dialog_, IDC_PREVIEW);
  status_ = GetDlgItem(dialog_, IDC_STATUS);
  application_path_ = GetDlgItem(dialog_, IDC_APPLICATION_PATH);
  ListView_SetExtendedListViewStyle(profile_list_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES |
                                                   LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
  LVCOLUMNW column{.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 198,
                   .pszText = const_cast<LPWSTR>(L"Profile")};
  ListView_InsertColumn(profile_list_, 0, &column);
  style_list_view(profile_list_, true);
  rebuild_profile_check_images();
  DrawMenuBar(dialog_);
  SetWindowSubclass(preview_, &EditorWindow::preview_subclass, 1,
                    reinterpret_cast<DWORD_PTR>(this));
  layout();
  reload_profile_list({}, true);
  set_status(L"Ready");
}

void EditorWindow::style_dialog_children(const HWND dialog) const {
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    std::array<wchar_t, 64> class_name{};
    GetClassNameW(child, class_name.data(), static_cast<int>(class_name.size()));
    if (wcscmp(class_name.data(), L"Button") == 0) {
      const auto style = GetWindowLongPtrW(child, GWL_STYLE);
      const auto button_type = style & static_cast<LONG_PTR>(BS_TYPEMASK);
      const bool is_checkbox =
          button_type == BS_CHECKBOX || button_type == BS_AUTOCHECKBOX ||
          button_type == BS_3STATE || button_type == BS_AUTO3STATE;
      const bool was_checked =
          is_checkbox && SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED;
      SetWindowLongPtrW(child, GWL_STYLE,
                        (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW);
      if (is_checkbox) set_owner_draw_checkbox(child, was_checked);
    } else if (wcscmp(class_name.data(), L"ComboBox") == 0) {
      SetWindowTheme(child, L"DarkMode_CFD", nullptr);
    } else if (wcscmp(class_name.data(), L"ListBox") == 0) {
      const auto style = GetWindowLongPtrW(child, GWL_STYLE);
      SetWindowLongPtrW(child, GWL_STYLE,
                        style | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
      const UINT dpi = GetDpiForWindow(dialog);
      SendMessageW(child, LB_SETITEMHEIGHT, 0,
                   static_cast<LPARAM>(MulDiv(18, dpi == 0 ? 96 : static_cast<int>(dpi), 96)));
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    } else if (wcscmp(class_name.data(), L"Edit") == 0 ||
               wcscmp(class_name.data(), WC_LISTVIEWW) == 0) {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
  }
}

void EditorWindow::style_list_view(const HWND list, const bool hide_header) const {
  if (list == nullptr) return;
  auto style = GetWindowLongPtrW(list, GWL_STYLE);
  if (hide_header) style |= LVS_NOCOLUMNHEADER;
  SetWindowLongPtrW(list, GWL_STYLE, style);
  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  ListView_SetBkColor(list, list_colors.fill);
  ListView_SetTextBkColor(list, list_colors.fill);
  ListView_SetTextColor(list, list_colors.text);
  SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
  if (const HWND header = ListView_GetHeader(list); header != nullptr)
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
  SetWindowPos(list, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void EditorWindow::rebuild_profile_check_images() {
  if (profile_list_ == nullptr) return;
  const HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR24, 2, 0);
  const HDC screen = GetDC(profile_list_);
  const HDC memory = screen == nullptr ? nullptr : CreateCompatibleDC(screen);
  if (images == nullptr || screen == nullptr || memory == nullptr) {
    if (memory != nullptr) DeleteDC(memory);
    if (screen != nullptr) ReleaseDC(profile_list_, screen);
    if (images != nullptr) ImageList_Destroy(images);
    return;
  }

  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  const auto check_colors = colors(layerc::platform::EditorControlRole::checkbox);
  for (const bool checked : {false, true}) {
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, 16, 16);
    const HGDIOBJ previous_bitmap = SelectObject(memory, bitmap);
    RECT image_rect{0, 0, 16, 16};
    const HBRUSH background = CreateSolidBrush(list_colors.fill);
    FillRect(memory, &image_rect, background);
    DeleteObject(background);

    RECT box{2, 2, 14, 14};
    const HBRUSH box_fill = CreateSolidBrush(checked ? check_colors.selection
                                                     : check_colors.hover);
    FillRect(memory, &box, box_fill);
    DeleteObject(box_fill);
    const HBRUSH border = CreateSolidBrush(check_colors.border);
    FrameRect(memory, &box, border);
    DeleteObject(border);
    if (checked) {
      const HPEN tick = CreatePen(PS_SOLID, 2, check_colors.text);
      const HGDIOBJ previous_pen = SelectObject(memory, tick);
      MoveToEx(memory, 4, 8, nullptr);
      LineTo(memory, 7, 11);
      LineTo(memory, 12, 5);
      SelectObject(memory, previous_pen);
      DeleteObject(tick);
    }
    SelectObject(memory, previous_bitmap);
    ImageList_Add(images, bitmap, nullptr);
    DeleteObject(bitmap);
  }
  DeleteDC(memory);
  ReleaseDC(profile_list_, screen);
  ImageList_SetBkColor(images, list_colors.fill);
  (void)ListView_SetImageList(profile_list_, images, LVSIL_STATE);
  if (profile_check_images_ != nullptr) ImageList_Destroy(profile_check_images_);
  profile_check_images_ = images;
}
void EditorWindow::rebuild_brushes() {
  for (auto &brush_value : brushes_) {
    if (brush_value != nullptr) DeleteObject(brush_value);
    brush_value = nullptr;
  }
  for (std::uint32_t index = 0;
       index < static_cast<std::uint32_t>(layerc::platform::EditorControlRole::count); ++index) {
    const auto role = static_cast<layerc::platform::EditorControlRole>(index);
    brushes_[index] = CreateSolidBrush(colors(role).fill);
  }
}

void EditorWindow::refresh_theme() {
  rebuild_brushes();
  style_list_view(profile_list_, true);
  rebuild_profile_check_images();
  style_list_view(task_list_, false);
  if (dialog_ != nullptr) {
    DrawMenuBar(dialog_);
    InvalidateRect(dialog_, nullptr, TRUE);
    EnumChildWindows(dialog_, [](HWND child, LPARAM) -> BOOL {
      InvalidateRect(child, nullptr, TRUE); return TRUE;
    }, 0);
  }
  if (task_dialog_ != nullptr) InvalidateRect(task_dialog_, nullptr, TRUE);
}

void EditorWindow::layout() {
  if (dialog_ == nullptr) return;
  RECT client{};
  GetClientRect(dialog_, &client);
  const int width = std::max(980, static_cast<int>(client.right - client.left));
  const int height = std::max(620, static_cast<int>(client.bottom - client.top));

  constexpr int margin = 12;
  constexpr int gap = 14;
  constexpr int toolbar_top = 8;
  constexpr int toolbar_height = 28;
  constexpr int section_top = 50;
  constexpr int heading_height = 18;
  constexpr int row_height = 24;
  const int status_top = height - 27;
  const int sidebar_actions_top = status_top - 36;
  const int sidebar_width = std::clamp(width / 5, 220, 270);
  const int content_left = margin + sidebar_width + 18;
  const int preview_width = std::clamp(width * 32 / 100, 330, 470);
  const int preview_left = width - margin - preview_width;
  const int content_right = preview_left - 18;
  const int content_width = std::max(360, content_right - content_left);

  const auto move = [&](const int id, const int x, const int y,
                        const int control_width, const int control_height) {
    if (const HWND control = GetDlgItem(dialog_, id); control != nullptr)
      MoveWindow(control, x, y, std::max(1, control_width),
                 std::max(1, control_height), TRUE);
  };

  int toolbar_x = 10;
  auto toolbar_button = [&](const int id, const int button_width) {
    move(id, toolbar_x, toolbar_top, button_width, toolbar_height);
    toolbar_x += button_width + 6;
  };
  toolbar_button(IDC_FILE_MENU, 50);
  toolbar_button(IDC_NEW_PROFILE, 56);
  toolbar_button(IDC_SAVE_PROFILE, 56);
  toolbar_button(IDC_ATTACH, 64);
  toolbar_button(IDC_SHOW_TASKS, 110);
  toolbar_button(IDC_SPLIT_BARS, 190);
  toolbar_button(IDC_OPTIONS, 76);

  move(IDC_SAVED_PROFILES_LABEL, margin, section_top, sidebar_width, heading_height);
  const int sidebar_list_top = section_top + heading_height + 4;
  move(IDC_PROFILE_LIST, margin, sidebar_list_top, sidebar_width,
       std::max(120, sidebar_actions_top - sidebar_list_top - 8));
  move(IDC_ATTACH_LEFT, margin, sidebar_actions_top, 82, 27);
  move(IDC_SHOW_TASKS_LEFT, margin + 88, sidebar_actions_top,
       sidebar_width - 88, 27);

  move(IDC_PROFILE_SECTION_LABEL, content_left, section_top,
       content_width, heading_height);
  constexpr int field_label_width = 86;
  const int field_left = content_left + field_label_width + 8;
  const int field_width = content_width - field_label_width - 8;
  const int profile_name_top = section_top + heading_height + 5;
  const int profile_id_top = profile_name_top + 29;
  const int application_top = profile_id_top + 29;
  move(IDC_PROFILE_NAME_LABEL, content_left, profile_name_top + 3,
       field_label_width, heading_height);
  move(IDC_PROFILE_NAME, field_left, profile_name_top, field_width, row_height);
  move(IDC_PROFILE_ID_LABEL, content_left, profile_id_top + 3,
       field_label_width, heading_height);
  move(IDC_PROFILE_ID, field_left, profile_id_top, field_width, row_height);
  move(IDC_APPLICATION_LABEL, content_left, application_top + 3,
       field_label_width, heading_height);
  move(IDC_APPLICATION_PATH, field_left, application_top, field_width, row_height);

  const int cooldown_heading_top = application_top + row_height + 17;
  const int cooldown_list_top = cooldown_heading_top + heading_height + 4;
  const int cooldown_button_width = 64;
  const int cooldown_list_width = content_width - cooldown_button_width - 8;
  move(IDC_COOLDOWNS_LABEL, content_left, cooldown_heading_top,
       content_width, heading_height);
  move(IDC_COOLDOWN_LIST, content_left, cooldown_list_top,
       cooldown_list_width, 112);
  move(IDC_ADD_COOLDOWN, content_left + cooldown_list_width + 8,
       cooldown_list_top, cooldown_button_width, 27);
  move(IDC_REMOVE_COOLDOWN, content_left + cooldown_list_width + 8,
       cooldown_list_top + 33, cooldown_button_width, 27);
  const int binding_top = cooldown_list_top + 119;
  constexpr int binding_label_width = 58;
  constexpr int capture_button_width = 108;
  move(IDC_COOLDOWN_BINDING_LABEL, content_left, binding_top + 3,
       binding_label_width, heading_height);
  move(IDC_COOLDOWN_BINDING, content_left + binding_label_width + 6,
       binding_top, content_width - binding_label_width - capture_button_width - 20,
       row_height);
  move(IDC_CAPTURE_BINDING, content_right - capture_button_width, binding_top,
       capture_button_width, row_height);

  const int bars_heading_top = binding_top + row_height + 18;
  const int bars_list_top = bars_heading_top + heading_height + 4;
  const int bar_list_width = std::clamp(content_width * 2 / 5, 185, 215);
  const int properties_left = content_left + bar_list_width + gap;
  const int properties_width = content_width - bar_list_width - gap;
  move(IDC_BARS_LABEL, content_left, bars_heading_top,
       content_width, heading_height);
  move(IDC_BAR_LIST, content_left, bars_list_top, bar_list_width, 134);
  move(IDC_SELECTED_BAR_LABEL, properties_left, bars_list_top,
       properties_width, heading_height);

  const int dimensions_top = bars_list_top + 27;
  const int dimension_half = (properties_width - 8) / 2;
  move(IDC_WIDTH_LABEL, properties_left, dimensions_top + 3, 40, heading_height);
  move(IDC_BAR_WIDTH, properties_left + 44, dimensions_top,
       std::max(46, dimension_half - 44), row_height);
  const int height_left = properties_left + dimension_half + 8;
  move(IDC_HEIGHT_LABEL, height_left, dimensions_top + 3, 42, heading_height);
  move(IDC_BAR_HEIGHT, height_left + 46, dimensions_top,
       std::max(46, properties_width - dimension_half - 54), row_height);

  const int color_button_width = (properties_width - 8) / 2;
  const int fill_button_width = std::min(color_button_width,
      std::max(110, (properties_width - 8) * 38 / 100));
  const int first_color_top = bars_list_top + 58;
  move(IDC_BAR_FILL, properties_left, first_color_top,
       fill_button_width, 27);
  move(IDC_BAR_BACKGROUND, properties_left + fill_button_width + 8,
       first_color_top, properties_width - fill_button_width - 8, 27);
  move(IDC_BAR_BORDER, properties_left, first_color_top + 33,
       color_button_width, 27);
  move(IDC_BAR_TEXT, properties_left + color_button_width + 8,
       first_color_top + 33, properties_width - color_button_width - 8, 27);
  move(IDC_HIDE_READY, properties_left, first_color_top + 67,
       properties_width, 25);
  move(IDC_POSITION_STATUS, content_left, bars_list_top + 153,
       content_width, 20);

  move(IDC_PREVIEW_LABEL, preview_left, section_top,
       preview_width, heading_height);
  const int preview_top = section_top + heading_height + 4;
  move(IDC_PREVIEW, preview_left, preview_top, preview_width,
       std::max(240, status_top - preview_top - 10));
  move(IDC_STATUS, margin, status_top, width - margin * 2, 19);
  ListView_SetColumnWidth(profile_list_, 0, sidebar_width - 8);
}
layerc::core::Panel *EditorWindow::selected_bar() {
  const LRESULT row = SendMessageW(bar_list_, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value = SendMessageW(bar_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 || static_cast<std::size_t>(value) >= profile_.panels.size()) return nullptr;
  return &profile_.panels[static_cast<std::size_t>(value)];
}

const layerc::core::Panel *EditorWindow::selected_bar() const {
  return const_cast<EditorWindow *>(this)->selected_bar();
}

void EditorWindow::refresh_profile_controls() {
  capturing_binding_ = false;
  syncing_ = true;
  set_text(GetDlgItem(dialog_, IDC_PROFILE_NAME), to_wide(profile_.name));
  set_text(GetDlgItem(dialog_, IDC_PROFILE_ID), to_wide(profile_.id));
  const auto application = profile_.game.executable_path.empty()
                               ? L"Choose a task from Detected Tasks or Pick Window."
                               : to_wide(profile_.game.executable_path);
  set_text(application_path_, application);
  refresh_cooldown_list();
  refresh_bar_list();
  syncing_ = false;
  InvalidateRect(preview_, nullptr, TRUE);
}

void EditorWindow::refresh_cooldown_list(const int selection) {
  SendMessageW(cooldown_list_, LB_RESETCONTENT, 0, 0);
  for (std::size_t index = 0; index < profile_.cooldowns.size(); ++index) {
    const auto &cooldown = profile_.cooldowns[index];
    const auto label = to_wide(cooldown.name) + L"  —  " +
                       std::to_wstring(cooldown.duration.count() / 1000) + L"." +
                       std::to_wstring((cooldown.duration.count() % 1000) / 100) + L" s";
    const auto item = SendMessageW(cooldown_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (item != LB_ERR && item != LB_ERRSPACE)
      SendMessageW(cooldown_list_, LB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(index));
  }
  if (!profile_.cooldowns.empty())
    SendMessageW(cooldown_list_, LB_SETCURSEL,
                 static_cast<WPARAM>(std::clamp(selection, 0, static_cast<int>(profile_.cooldowns.size()) - 1)), 0);
  refresh_binding_controls();
}

layerc::core::CooldownDefinition *EditorWindow::selected_cooldown() {
  const LRESULT row = SendMessageW(cooldown_list_, LB_GETCURSEL, 0, 0);
  if (row == LB_ERR) return nullptr;
  const LRESULT value =
      SendMessageW(cooldown_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (value == LB_ERR || value < 0 ||
      static_cast<std::size_t>(value) >= profile_.cooldowns.size()) return nullptr;
  return &profile_.cooldowns[static_cast<std::size_t>(value)];
}

void EditorWindow::refresh_binding_controls() {
  const auto *cooldown = selected_cooldown();
  const HWND binding = GetDlgItem(dialog_, IDC_COOLDOWN_BINDING);
  const HWND capture = GetDlgItem(dialog_, IDC_CAPTURE_BINDING);
  EnableWindow(binding, cooldown != nullptr);
  EnableWindow(capture, cooldown != nullptr);
  set_text(capture, capturing_binding_ ? L"Press key..." : L"Capture key");
  if (cooldown == nullptr) {
    set_text(binding, L"Select a cooldown");
    return;
  }
  if (capturing_binding_) {
    set_text(binding, L"Press a key (Esc cancels)");
    return;
  }

  const layerc::core::InputBinding *only{};
  std::size_t count{};
  for (const auto &candidate : profile_.bindings) {
    if (candidate.cooldown_id != cooldown->id) continue;
    only = &candidate;
    ++count;
  }
  if (count != 1 || only == nullptr || only->chord.tokens.size() != 1) {
    set_text(binding, count == 0 ? L"Unbound" : L"Custom chord — capture replaces it");
    return;
  }

  const auto &token = only->chord.tokens.front();
  std::wstring label;
  if (token.device == layerc::core::InputDevice::keyboard) {
    const LONG key_data = static_cast<LONG>((token.code & 0x00FFU) << 16U) |
                          ((token.code & 0xE000U) != 0 ? (1L << 24) : 0L);
    std::array<wchar_t, 128> name{};
    if (GetKeyNameTextW(key_data, name.data(), static_cast<int>(name.size())) > 0)
      label = name.data();
    else
      label = L"Keyboard key";
    wchar_t code[24]{};
    swprintf_s(code, L"  (scan 0x%04X)", token.code);
    label += code;
  } else if (token.device == layerc::core::InputDevice::mouse) {
    label = L"Mouse button " + std::to_wstring(token.code);
  } else {
    label = L"Controller " + std::to_wstring(token.controller_index + 1U) +
            L", input " + std::to_wstring(token.code);
  }
  set_text(binding, label);
}

void EditorWindow::begin_binding_capture() {
  if (selected_cooldown() == nullptr) {
    set_status(L"Select a cooldown before capturing a binding.");
    return;
  }
  capturing_binding_ = true;
  SetFocus(GetDlgItem(dialog_, IDC_CAPTURE_BINDING));
  refresh_binding_controls();
  set_status(L"Press a key for the selected cooldown; Esc cancels.");
}

void EditorWindow::cancel_binding_capture(const std::wstring_view status) {
  if (!capturing_binding_) return;
  capturing_binding_ = false;
  refresh_binding_controls();
  if (!status.empty()) set_status(status);
}

void EditorWindow::assign_binding_key(const LPARAM key_data) {
  auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) {
    cancel_binding_capture(L"Binding capture cancelled because no cooldown is selected.");
    return;
  }
  const auto message_data = static_cast<std::uintptr_t>(key_data);
  std::uint16_t code =
      static_cast<std::uint16_t>((message_data >> 16U) & 0x00FFU);
  if ((message_data & (1ULL << 24U)) != 0)
    code = static_cast<std::uint16_t>(code | 0xE000U);
  if ((code & 0x00FFU) == 0) {
    set_status(L"That key cannot be represented as a Windows scan code; try another key.");
    return;
  }

  const auto cooldown_id = cooldown->id;
  std::erase_if(profile_.bindings, [&](const layerc::core::InputBinding &binding) {
    return binding.cooldown_id == cooldown_id;
  });
  profile_.bindings.push_back({
      .cooldown_id = cooldown_id,
      .chord = layerc::core::InputChord{{{
          layerc::core::InputDevice::keyboard, code, 0}}}});
  dirty_ = true;
  capturing_binding_ = false;
  refresh_binding_controls();
  set_status(L"Binding updated. Save or Attach to apply it.");
}

void EditorWindow::refresh_bar_list(const std::size_t preferred_panel) {
  SendMessageW(bar_list_, LB_RESETCONTENT, 0, 0);
  int selected_row = -1;
  for (std::size_t index = 0; index < profile_.panels.size(); ++index) {
    const auto &panel = profile_.panels[index];
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    std::wstring label = to_wide(panel.name);
    if (panel.cooldown_ids.size() > 1)
      label += L"  (grouped: " + std::to_wstring(panel.cooldown_ids.size()) + L")";
    else if (!panel.cooldown_ids.empty()) label += L"  —  " + to_wide(panel.cooldown_ids.front());
    const auto row = SendMessageW(bar_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (row != LB_ERR && row != LB_ERRSPACE) {
      SendMessageW(bar_list_, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(index));
      if (index == preferred_panel) selected_row = static_cast<int>(row);
    }
  }
  if (selected_row < 0 && SendMessageW(bar_list_, LB_GETCOUNT, 0, 0) > 0) selected_row = 0;
  if (selected_row >= 0) SendMessageW(bar_list_, LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
  refresh_bar_controls();
}

void EditorWindow::refresh_bar_controls() {
  syncing_ = true;
  const auto *panel = selected_bar();
  bar_controls_panel_ = panel == nullptr
                            ? SIZE_MAX
                            : static_cast<std::size_t>(panel - profile_.panels.data());
  for (const int id : {IDC_BAR_WIDTH, IDC_BAR_HEIGHT, IDC_BAR_FILL, IDC_BAR_BACKGROUND,
                       IDC_BAR_BORDER, IDC_BAR_TEXT, IDC_HIDE_READY})
    EnableWindow(GetDlgItem(dialog_, id), panel != nullptr);
  if (panel != nullptr) {
    set_text(GetDlgItem(dialog_, IDC_BAR_WIDTH), std::to_wstring(panel->bar.width));
    set_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT), std::to_wstring(panel->bar.height));
    set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_HIDE_READY),
                            panel->bar.hide_when_ready);
    set_text(GetDlgItem(dialog_, IDC_POSITION_STATUS), panel->cooldown_ids.size() > 1
      ? L"Legacy grouped bar. Split it to give each cooldown its own position."
      : L"Drag the selected bar in the preview to save normalized position.");
    set_text(GetDlgItem(dialog_, IDC_BAR_FILL), color_label(L"Fill", panel->bar.fill_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_BACKGROUND), color_label(L"Background", panel->bar.background_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_BORDER), color_label(L"Border", panel->bar.border_color));
    set_text(GetDlgItem(dialog_, IDC_BAR_TEXT), color_label(L"Text", panel->bar.text_color));
  } else set_text(GetDlgItem(dialog_, IDC_POSITION_STATUS), L"Add a cooldown to create a bar.");
  syncing_ = false;
  InvalidateRect(preview_, nullptr, TRUE);
}

void EditorWindow::sync_profile_from_controls() {
  if (syncing_) return;
  profile_.name = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_NAME)));
  profile_.id = to_utf8(control_text(GetDlgItem(dialog_, IDC_PROFILE_ID)));
  if (bar_controls_panel_ < profile_.panels.size()) {
    auto &panel = profile_.panels[bar_controls_panel_];
    panel.bar.width = parse_uint(control_text(GetDlgItem(dialog_, IDC_BAR_WIDTH)), panel.bar.width);
    panel.bar.height = parse_uint(control_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT)), panel.bar.height);
    panel.bar.hide_when_ready =
        owner_draw_checkbox_checked(GetDlgItem(dialog_, IDC_HIDE_READY));
  }
}

bool EditorWindow::validate_profile() {
  sync_profile_from_controls();
  const auto parsed = layerc::core::parse_profile_json(layerc::core::serialize_profile_json(profile_));
  if (const auto *error = std::get_if<layerc::core::ProfileError>(&parsed)) {
    MessageBoxW(dialog_, profile_error_text(*error).c_str(), L"Profile is incomplete", MB_OK | MB_ICONWARNING);
    return false;
  }
  profile_ = std::get<layerc::core::GameProfile>(parsed);
  return true;
}

bool EditorWindow::confirm_unsaved_changes() {
  if (!dirty_) return true;
  const int choice = MessageBoxW(dialog_, L"The current profile has unsaved changes. Save them first?",
                                 L"Unsaved changes", MB_YESNOCANCEL | MB_ICONWARNING);
  return choice == IDYES ? save_profile(false) : choice == IDNO;
}

void EditorWindow::new_profile(const bool confirm) {
  if (confirm && !confirm_unsaved_changes()) return;
  profile_ = make_new_profile();
  current_path_.clear();
  dirty_ = true;
  refresh_profile_controls();
  set_status(L"New profile ready — choose a task, configure bars, then Attach.");
}
void EditorWindow::reload_profile_list(const std::filesystem::path &select_path,
                                       const bool open_selection) {
  entries_.clear();
  syncing_ = true;
  ListView_DeleteAllItems(profile_list_);
  const auto directory = profile_directory();
  std::error_code error;
  if (!directory.empty() && std::filesystem::exists(directory, error) && !error) {
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
      if (error || !entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".json") != 0) continue;
      const auto contents = read_profile_file(entry.path());
      if (!contents.has_value()) continue;
      const auto parsed = layerc::core::parse_profile_json(*contents);
      const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
      if (profile == nullptr) continue;
      entries_.push_back({.path = entry.path(),
                          .label = profile_list_label(*profile),
                          .enabled = profile->enabled});
    }
  }
  std::sort(entries_.begin(), entries_.end(), [](const ProfileEntry &left, const ProfileEntry &right) {
    return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
  });
  int selected = -1;
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    LVITEMW item{.mask = LVIF_TEXT | LVIF_PARAM, .iItem = static_cast<int>(index),
                 .pszText = const_cast<LPWSTR>(entries_[index].label.c_str()),
                 .lParam = static_cast<LPARAM>(index)};
    ListView_InsertItem(profile_list_, &item);
    ListView_SetCheckState(profile_list_, static_cast<int>(index), entries_[index].enabled);
    if (!select_path.empty() && entries_[index].path.lexically_normal() == select_path.lexically_normal())
      selected = static_cast<int>(index);
  }
  if (selected < 0 && !entries_.empty()) selected = 0;
  if (selected >= 0) ListView_SetItemState(profile_list_, selected, LVIS_SELECTED | LVIS_FOCUSED,
                                             LVIS_SELECTED | LVIS_FOCUSED);
  syncing_ = false;
  if (open_selection && selected >= 0) open_profile_row(selected, false);
  else if (entries_.empty() && current_path_.empty()) new_profile(false);
}

void EditorWindow::open_profile_row(const int row, const bool confirm) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) return;
  const auto target_path = entries_[static_cast<std::size_t>(row)].path;
  const auto select_path = [&](const std::filesystem::path &path) {
    syncing_ = true;
    ListView_SetItemState(profile_list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].path.lexically_normal() != path.lexically_normal()) continue;
      ListView_SetItemState(profile_list_, static_cast<int>(index),
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(profile_list_, static_cast<int>(index), FALSE);
      break;
    }
    syncing_ = false;
  };
  if (confirm && !confirm_unsaved_changes()) {
    select_path(current_path_);
    return;
  }

  const auto contents = read_profile_file(target_path);
  if (!contents.has_value()) {
    select_path(current_path_);
    set_status(L"Could not read the selected profile.");
    return;
  }
  const auto parsed = layerc::core::parse_profile_json(*contents);
  const auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (profile == nullptr) {
    select_path(current_path_);
    set_status(L"The selected profile is invalid.");
    return;
  }
  profile_ = *profile;
  current_path_ = target_path;
  dirty_ = false;
  select_path(target_path);
  refresh_profile_controls();
  set_status(profile_.enabled ? L"Profile loaded." : L"Disabled profile loaded.");
}

void EditorWindow::save_profile_enabled(const int row, const bool enabled) {
  if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) return;
  auto &entry = entries_[static_cast<std::size_t>(row)];
  const auto restore_checkbox = [&] {
    const bool was_syncing = syncing_;
    syncing_ = true;
    ListView_SetCheckState(profile_list_, row, entry.enabled);
    syncing_ = was_syncing;
  };
  const auto contents = read_profile_file(entry.path);
  if (!contents.has_value()) {
    restore_checkbox();
    set_status(L"Could not update the selected profile.");
    return;
  }
  auto parsed = layerc::core::parse_profile_json(*contents);
  auto *stored = std::get_if<layerc::core::GameProfile>(&parsed);
  if (stored == nullptr) {
    restore_checkbox();
    set_status(L"Could not update an invalid profile.");
    return;
  }
  stored->enabled = enabled;
  if (!write_profile_file(entry.path, layerc::core::serialize_profile_json(*stored))) {
    restore_checkbox();
    set_status(L"Could not save the profile enablement change.");
    return;
  }
  entry.enabled = enabled;
  entry.label = profile_list_label(*stored);
  const bool was_syncing = syncing_;
  syncing_ = true;
  ListView_SetItemText(profile_list_, row, 0, entry.label.data());
  syncing_ = was_syncing;
  if (entry.path.lexically_normal() == current_path_.lexically_normal())
    profile_.enabled = enabled;
  (void)signal_profile_reload();
  set_status(enabled ? L"Profile enabled — LayerD reloaded."
                     : L"Profile disabled — LayerD retired its instances.");
  InvalidateRect(profile_list_, nullptr, TRUE);
}

bool EditorWindow::choose_save_path(std::filesystem::path &path) {
  const auto directory = profile_directory();
  if (directory.empty()) {
    MessageBoxW(dialog_, L"The profiles directory is unavailable.", L"LayerE",
                MB_OK | MB_ICONERROR);
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    MessageBoxW(dialog_, L"Could not create the profiles directory.", L"LayerE",
                MB_OK | MB_ICONERROR);
    return false;
  }

  const auto initial_filename = path.empty()
      ? std::filesystem::path(to_wide(safe_identifier(to_wide(profile_.id))) + L".json")
      : path.filename();
  auto initial_path = directory / initial_filename;
  const auto initial_directory = directory.wstring();
  for (;;) {
    std::array<wchar_t, 32'768> buffer{};
    wcsncpy_s(buffer.data(), buffer.size(), initial_path.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = this->dialog_;
    dialog.lpstrFilter = L"LayerC profiles (*.json)\0*.json\0All files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = initial_directory.c_str();
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog) == FALSE) return false;

    auto candidate = std::filesystem::path(buffer.data());
    if (candidate.extension().empty()) candidate += L".json";
    if (_wcsicmp(candidate.extension().c_str(), L".json") != 0) {
      candidate.replace_extension(L".json");
      MessageBoxW(dialog_, L"LayerC profiles must use the .json extension.",
                  L"Choose a JSON profile name", MB_OK | MB_ICONWARNING);
      initial_path = directory / candidate.filename();
      continue;
    }
    std::error_code directory_error;
    std::error_code parent_error;
    const auto canonical_directory =
        std::filesystem::weakly_canonical(directory, directory_error);
    const auto canonical_parent =
        std::filesystem::weakly_canonical(candidate.parent_path(), parent_error);
    if (!directory_error && !parent_error &&
        _wcsicmp(canonical_directory.c_str(), canonical_parent.c_str()) == 0) {
      path = std::move(candidate);
      return true;
    }

    MessageBoxW(dialog_,
                L"Profiles must be saved directly in the LayerC profiles folder so LayerD can load them.",
                L"Choose the profiles folder", MB_OK | MB_ICONWARNING);
    initial_path = directory / candidate.filename();
  }
}

bool EditorWindow::save_profile(const bool force_save_as) {
  if (!validate_profile()) return false;
  auto destination = current_path_;
  if (force_save_as) {
    if (!choose_save_path(destination)) return false;
  } else if (destination.empty()) {
    destination = profile_directory() / (to_wide(safe_identifier(to_wide(profile_.id))) + L".json");
    std::error_code error;
    if (std::filesystem::exists(destination, error) &&
        MessageBoxW(dialog_, L"A profile with this ID already exists. Overwrite it?",
                    L"Profile already exists", MB_YESNO | MB_ICONWARNING) != IDYES) return false;
    if (error) { MessageBoxW(dialog_, L"Could not inspect the profile destination.", L"LayerE", MB_OK | MB_ICONERROR); return false; }
  }
  if (!write_profile_file(destination, layerc::core::serialize_profile_json(profile_))) {
    MessageBoxW(dialog_, L"Could not save the profile file.", L"LayerE", MB_OK | MB_ICONERROR);
    return false;
  }
  current_path_ = destination;
  dirty_ = false;
  reload_profile_list(current_path_, false);
  (void)signal_profile_reload();
  set_status(L"Profile saved — LayerD reloaded.");
  return true;
}

void EditorWindow::delete_profile() {
  if (current_path_.empty()) {
    MessageBoxW(dialog_, L"Save this profile before deleting it.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto prompt = L"Move '" + to_wide(profile_.name) + L"' to the Recycle Bin?";
  if (MessageBoxW(dialog_, prompt.c_str(), L"Delete profile", MB_YESNO | MB_ICONWARNING) != IDYES) return;
  std::filesystem::path safe_remaining;
  for (const auto &entry : entries_) {
    if (entry.path.lexically_normal() != current_path_.lexically_normal()) { safe_remaining = entry.path; break; }
  }
  if (!recycle_profile_file(current_path_)) {
    MessageBoxW(dialog_, L"Could not move the profile to the Recycle Bin.", L"LayerE", MB_OK | MB_ICONERROR);
    return;
  }
  current_path_.clear(); dirty_ = false;
  (void)signal_profile_reload();
  reload_profile_list(safe_remaining, true);
  set_status(safe_remaining.empty() ? L"Profile moved to the Recycle Bin."
                                    : L"Profile deleted and LayerD reloaded.");
}

void EditorWindow::open_profiles_folder() {
  const auto directory = profile_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || directory.empty()) {
    MessageBoxW(dialog_, L"Could not open the profiles folder.", L"LayerE", MB_OK | MB_ICONERROR);
    return;
  }
  ShellExecuteW(dialog_, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void EditorWindow::show_file_menu() {
  const HMENU menu = LoadMenuW(instance_, MAKEINTRESOURCEW(IDR_EDITOR_MENU));
  const HMENU popup = menu == nullptr ? nullptr : GetSubMenu(menu, 0);
  if (popup == nullptr) {
    if (menu != nullptr) DestroyMenu(menu);
    set_status(L"Could not open the File menu.");
    return;
  }
  RECT button{};
  GetWindowRect(GetDlgItem(dialog_, IDC_FILE_MENU), &button);
  SetForegroundWindow(dialog_);
  TrackPopupMenuEx(popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON |
                              TPM_RIGHTBUTTON,
                   button.left, button.bottom, dialog_, nullptr);
  PostMessageW(dialog_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}
void EditorWindow::attach_profile() {
  if (!profile_.enabled) {
    MessageBoxW(dialog_,
                L"This profile is disabled. Enable it in Saved Profiles before attaching.",
                L"Profile disabled", MB_OK | MB_ICONINFORMATION);
    set_status(L"Profile is disabled — enable it before attaching.");
    return;
  }
  if (!save_profile(false)) return;
  if (signal_profile_reload()) set_status(L"Saved — LayerD reloaded and will attach automatically.");
  else if (start_daemon()) set_status(L"Saved — LayerD started and will attach automatically.");
  else MessageBoxW(dialog_, L"Profile was saved, but LayerD could not be started.", L"LayerE", MB_OK | MB_ICONWARNING);
}

void EditorWindow::initialize_tasks(const HWND window) {
  task_dialog_ = window;
  set_dark_title_bar(window); style_dialog_children(window);
  task_list_ = GetDlgItem(window, IDC_TASK_LIST);
  ListView_SetExtendedListViewStyle(task_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const std::array<LVCOLUMNW, 3> columns{{
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 145, .pszText = const_cast<LPWSTR>(L"Process")},
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 335, .pszText = const_cast<LPWSTR>(L"Window")},
      {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 80, .pszText = const_cast<LPWSTR>(L"PID")},
  }};
  for (int index = 0; index < static_cast<int>(columns.size()); ++index)
    ListView_InsertColumn(task_list_, index, const_cast<LVCOLUMNW *>(&columns[static_cast<std::size_t>(index)]));
  style_list_view(task_list_, false);
  refresh_tasks();
}

void EditorWindow::refresh_tasks() {
  if (task_list_ == nullptr) return;
  const HWND previous = selected_task_window();
  task_candidates_.clear(); ListView_DeleteAllItems(task_list_);
  for (auto candidate : layerc::platform::enumerate_attachable_windows()) {
    if (candidate.process_id == GetCurrentProcessId()) continue;
    task_candidates_.push_back(std::move(candidate));
    const auto &item = task_candidates_.back();
    const int row = static_cast<int>(task_candidates_.size() - 1U);
    LVITEMW list_item{.mask = LVIF_TEXT | LVIF_PARAM, .iItem = row,
                      .pszText = const_cast<LPWSTR>(item.executable_name.c_str()), .lParam = static_cast<LPARAM>(row)};
    ListView_InsertItem(task_list_, &list_item);
    ListView_SetItemText(task_list_, row, 1, const_cast<LPWSTR>(item.title.empty() ? L"(untitled)" : item.title.c_str()));
    const auto pid = std::to_wstring(item.process_id);
    ListView_SetItemText(task_list_, row, 2, const_cast<LPWSTR>(pid.c_str()));
    if (item.window == previous) ListView_SetItemState(task_list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                                                        LVIS_SELECTED | LVIS_FOCUSED);
  }
  update_task_detail();
}

HWND EditorWindow::selected_task_window() const {
  if (task_list_ == nullptr) return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
      ? task_candidates_[static_cast<std::size_t>(row)].window : nullptr;
}

const layerc::platform::WindowCandidate *EditorWindow::selected_task() const {
  if (task_list_ == nullptr) return nullptr;
  const int row = ListView_GetNextItem(task_list_, -1, LVNI_SELECTED);
  return row >= 0 && static_cast<std::size_t>(row) < task_candidates_.size()
      ? &task_candidates_[static_cast<std::size_t>(row)] : nullptr;
}

void EditorWindow::update_task_detail() {
  if (task_dialog_ == nullptr) return;
  const auto *candidate = selected_task();
  set_text(GetDlgItem(task_dialog_, IDC_TASK_DETAIL), candidate == nullptr
      ? L"Select a task to see its executable path." : candidate->executable_path);
}

void EditorWindow::apply_task(const layerc::platform::WindowCandidate &candidate) {
  sync_profile_from_controls();
  profile_.game.global = false;
  profile_.game.executable_path = to_utf8(candidate.executable_path);
  profile_.game.executable_name = to_utf8(candidate.executable_name);
  profile_.game.window_class.clear(); profile_.game.window_title.clear();
  if (profile_.name == "New Profile" || profile_.name.empty()) {
    const auto suggested = std::filesystem::path(candidate.executable_name).stem().wstring();
    profile_.name = to_utf8(suggested); profile_.id = safe_identifier(suggested);
  }
  dirty_ = true; refresh_profile_controls(); update_task_detail();
  set_status(L"Task selected. Attach saves a durable executable match.");
}

void EditorWindow::use_selected_task() {
  const auto *candidate = selected_task();
  if (candidate == nullptr) {
    MessageBoxW(task_dialog_, L"Select a visible task first.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  apply_task(*candidate);
}

void EditorWindow::show_tasks() {
  if (task_dialog_ == nullptr || !IsWindow(task_dialog_)) {
    task_dialog_ = CreateDialogParamW(instance_, MAKEINTRESOURCEW(IDD_TASKS), dialog_,
                                      &EditorWindow::task_dialog_proc, reinterpret_cast<LPARAM>(this));
  }
  if (task_dialog_ == nullptr) { set_status(L"Could not open the task picker."); return; }
  refresh_tasks(); set_dark_title_bar(task_dialog_);
  (void)layerc::platform::apply_capture_exclusion(task_dialog_, capture_exclusion_);
  ShowWindow(task_dialog_, SW_SHOWNORMAL); SetForegroundWindow(task_dialog_);
}

void EditorWindow::begin_picker() {
  if (picking_) { cancel_picker(); return; }
  picking_ = true;
  task_was_visible_ = task_dialog_ != nullptr && IsWindowVisible(task_dialog_);
  if (task_was_visible_) ShowWindow(task_dialog_, SW_HIDE);
  SetCapture(dialog_);
  set_status(L"Pick Window active — click a game window, or press Esc to cancel.");
}

void EditorWindow::finish_picker() {
  POINT cursor{}; GetCursorPos(&cursor);
  const auto candidate = layerc::platform::window_at_point(cursor);
  cancel_picker();
  if (!candidate.has_value() || candidate->window == dialog_) { set_status(L"No eligible window was picked."); return; }
  apply_task(*candidate);
}

void EditorWindow::cancel_picker() {
  if (!picking_) return;
  picking_ = false;
  if (GetCapture() == dialog_) ReleaseCapture();
  if (task_was_visible_ && task_dialog_ != nullptr) ShowWindow(task_dialog_, SW_SHOWNORMAL);
  task_was_visible_ = false;
}
void EditorWindow::add_cooldown() {
  sync_profile_from_controls();
  int number = 1;
  std::string id;
  do {
    id = "ability-" + std::to_string(number++);
  } while (std::any_of(profile_.cooldowns.begin(), profile_.cooldowns.end(),
                       [&](const layerc::core::CooldownDefinition &cooldown) {
                         return cooldown.id == id;
                       }));
  const int display_number = number - 1;
  profile_.cooldowns.push_back({.id = id, .name = "Ability " + std::to_string(display_number),
                                .duration = std::chrono::seconds(5), .max_charges = 1,
                                .retrigger = layerc::core::RetriggerPolicy::ignore});

  constexpr std::array<std::uint16_t, 36> preferred_scan_codes{
      0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
      0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
      0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32,
      0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x3B, 0x3C};
  const auto scan_code_used = [&](const std::uint16_t candidate) {
    return std::any_of(profile_.bindings.begin(), profile_.bindings.end(),
                       [&](const layerc::core::InputBinding &binding) {
      return std::any_of(binding.chord.tokens.begin(), binding.chord.tokens.end(),
                         [&](const layerc::core::InputToken &token) {
        return token.device == layerc::core::InputDevice::keyboard &&
               token.code == candidate;
      });
    });
  };
  std::uint16_t scan_code{};
  for (const auto candidate : preferred_scan_codes) {
    if (!scan_code_used(candidate)) {
      scan_code = candidate;
      break;
    }
  }
  if (scan_code == 0) {
    for (std::uint16_t candidate = 1; candidate < 0x80; ++candidate) {
      if (!scan_code_used(candidate)) {
        scan_code = candidate;
        break;
      }
    }
  }
  if (scan_code == 0) scan_code = 0x12;
  profile_.bindings.push_back({.cooldown_id = id,
      .chord = layerc::core::InputChord{{{layerc::core::InputDevice::keyboard, scan_code, 0}}}});
  const bool independent = !profile_.panels.empty() && std::all_of(
      profile_.panels.begin(), profile_.panels.end(), [](const layerc::core::Panel &panel) {
        return panel.style == layerc::core::PanelStyle::bar && panel.cooldown_ids.size() == 1;
      });
  if (independent) {
    layerc::core::Panel panel;
    panel.id = unique_panel_id(id + "-bar", profile_.panels);
    panel.name = "Ability " + std::to_string(display_number);
    panel.style = layerc::core::PanelStyle::bar;
    panel.anchor_x = 0.5;
    panel.anchor_y = std::clamp(0.2 + 0.10 * static_cast<double>(display_number), 0.0, 1.0);
    panel.cooldown_ids = {id};
    profile_.panels.push_back(std::move(panel));
    refresh_bar_list(profile_.panels.size() - 1U);
  } else {
    if (profile_.panels.empty()) profile_.panels.push_back({.id = "main", .name = "Main",
        .style = layerc::core::PanelStyle::bar, .cooldown_ids = {}});
    profile_.panels.front().cooldown_ids.push_back(id);
    refresh_bar_list();
  }
  dirty_ = true; refresh_cooldown_list(static_cast<int>(profile_.cooldowns.size()) - 1);
  set_status(L"Cooldown added with an unused keyboard binding; Capture key can change it.");
}

void EditorWindow::remove_cooldown() {
  if (profile_.cooldowns.size() <= 1) {
    MessageBoxW(dialog_, L"A profile needs at least one cooldown.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const LRESULT row = SendMessageW(cooldown_list_, LB_GETCURSEL, 0, 0);
  const LRESULT data = row == LB_ERR ? LB_ERR :
      SendMessageW(cooldown_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  if (data == LB_ERR || data < 0 || static_cast<std::size_t>(data) >= profile_.cooldowns.size()) {
    MessageBoxW(dialog_, L"Select a cooldown to remove.", L"LayerE", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto &cooldown = profile_.cooldowns[static_cast<std::size_t>(data)];
  const auto prompt = L"Remove '" + to_wide(cooldown.name) + L"'?";
  if (MessageBoxW(dialog_, prompt.c_str(), L"Remove cooldown", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  const auto id = cooldown.id;
  profile_.cooldowns.erase(profile_.cooldowns.begin() + data);
  std::erase_if(profile_.bindings, [&](const layerc::core::InputBinding &binding) { return binding.cooldown_id == id; });
  std::erase_if(profile_.panels, [&](layerc::core::Panel &panel) {
    std::erase(panel.cooldown_ids, id); return panel.cooldown_ids.empty();
  });
  if (profile_.panels.empty()) profile_.panels.push_back({.id = "main", .name = "Main",
      .style = layerc::core::PanelStyle::bar, .cooldown_ids = {profile_.cooldowns.front().id}});
  dirty_ = true; refresh_cooldown_list(static_cast<int>(data) - 1); refresh_bar_list();
  set_status(L"Cooldown removed.");
}

void EditorWindow::split_independent_bars() {
  sync_profile_from_controls();
  std::vector<layerc::core::Panel> split;
  split.reserve(profile_.panels.size() + profile_.cooldowns.size());
  bool changed{}; std::size_t first_split = SIZE_MAX;
  for (const auto &panel : profile_.panels) {
    if (panel.style != layerc::core::PanelStyle::bar || panel.cooldown_ids.size() <= 1) {
      split.push_back(panel); continue;
    }
    changed = true;
    const double offset = (static_cast<double>(panel.bar.height) + 8.0) / 900.0;
    for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index) {
      auto bar = panel;
      const auto &cooldown = panel.cooldown_ids[index];
      bar.id = unique_panel_id(panel.id + "-" + cooldown, profile_.panels, &split);
      bar.name = panel.name + " " + cooldown;
      bar.cooldown_ids = {cooldown};
      bar.anchor_y = std::clamp(panel.anchor_y +
          (static_cast<double>(index) - (static_cast<double>(panel.cooldown_ids.size()) - 1.0) / 2.0) * offset,
          0.0, 1.0);
      if (first_split == SIZE_MAX) first_split = split.size();
      split.push_back(std::move(bar));
    }
  }
  if (!changed) { set_status(L"Bars are already independent; legacy groups were left unchanged."); return; }
  profile_.panels = std::move(split); dirty_ = true;
  refresh_bar_list(first_split);
  set_status(L"Split grouped bars into independently positioned cooldown bars.");
}

void EditorWindow::choose_bar_color(const int id) {
  sync_profile_from_controls();
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  std::uint32_t *value{};
  switch (id) {
  case IDC_BAR_FILL: value = &panel->bar.fill_color; break;
  case IDC_BAR_BACKGROUND: value = &panel->bar.background_color; break;
  case IDC_BAR_BORDER: value = &panel->bar.border_color; break;
  case IDC_BAR_TEXT: value = &panel->bar.text_color; break;
  default: return;
  }
  CHOOSECOLORW choose{};
  choose.lStructSize = sizeof(choose); choose.hwndOwner = dialog_; choose.rgbResult = color_ref(*value);
  choose.lpCustColors = custom_colors_.data(); choose.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (ChooseColorW(&choose) == FALSE) return;
  *value = rgb_value(choose.rgbResult); dirty_ = true; refresh_bar_controls();
}

double EditorWindow::preview_aspect_ratio() const {
  const auto expected_path = to_wide(profile_.game.executable_path);
  const auto expected_name = to_wide(profile_.game.executable_name);
  for (const auto &candidate : task_candidates_) {
    const bool path_matches = !expected_path.empty() &&
                              _wcsicmp(candidate.executable_path.c_str(), expected_path.c_str()) == 0;
    const bool name_matches = !expected_name.empty() &&
                              _wcsicmp(candidate.executable_name.c_str(), expected_name.c_str()) == 0;
    if (!path_matches && !name_matches) continue;
    RECT client{};
    if (GetClientRect(candidate.window, &client) && client.right > client.left && client.bottom > client.top)
      return static_cast<double>(client.right - client.left) / static_cast<double>(client.bottom - client.top);
  }
  return 16.0 / 9.0;
}

RECT EditorWindow::preview_canvas() const {
  RECT client{}; GetClientRect(preview_, &client);
  constexpr int inset = 12;
  const int available_width = std::max(1L, client.right - client.left - inset * 2);
  const int available_height = std::max(1L, client.bottom - client.top - inset * 2);
  const double aspect = preview_aspect_ratio();
  int width = available_width;
  int height = static_cast<int>(std::lround(width / aspect));
  if (height > available_height) { height = available_height; width = static_cast<int>(std::lround(height * aspect)); }
  const int left = (client.right - width) / 2;
  const int top = (client.bottom - height) / 2;
  return {left, top, left + width, top + height};
}

RECT EditorWindow::bar_preview_rect(const layerc::core::Panel &panel, const RECT &canvas) const {
  const auto canvas_width = std::max(1L, canvas.right - canvas.left);
  const auto canvas_height = std::max(1L, canvas.bottom - canvas.top);
  const auto scale = std::min(static_cast<double>(canvas_width) / 1920.0,
                              static_cast<double>(canvas_height) / 1080.0);
  const int width = std::max(28, static_cast<int>(std::lround(panel.bar.width * scale)));
  const int height = std::max(14, static_cast<int>(std::lround(panel.bar.height * scale)));
  const int center_x = canvas.left + static_cast<int>(std::lround(panel.anchor_x * canvas_width));
  const int center_y = canvas.top + static_cast<int>(std::lround(panel.anchor_y * canvas_height));
  return {center_x - width / 2, center_y - height / 2, center_x - width / 2 + width, center_y - height / 2 + height};
}

void EditorWindow::paint_preview(const HWND window) {
  PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
  const auto preview_colors = colors(layerc::platform::EditorControlRole::preview);
  RECT client{}; GetClientRect(window, &client);
  const HBRUSH fill = CreateSolidBrush(preview_colors.fill); FillRect(dc, &client, fill); DeleteObject(fill);
  const RECT canvas = preview_canvas();
  const HBRUSH canvas_fill = CreateSolidBrush(colors(layerc::platform::EditorControlRole::window).fill);
  FillRect(dc, &canvas, canvas_fill); DeleteObject(canvas_fill);
  const HPEN canvas_pen = CreatePen(PS_SOLID, 1, preview_colors.border);
  const HGDIOBJ old_pen = SelectObject(dc, canvas_pen); const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
  SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(canvas_pen);
  const auto *selected = selected_bar();
  for (const auto &panel : profile_.panels) {
    if (panel.style != layerc::core::PanelStyle::bar) continue;
    const RECT bar = bar_preview_rect(panel, canvas);
    const HBRUSH background = CreateSolidBrush(color_ref(panel.bar.background_color)); FillRect(dc, &bar, background); DeleteObject(background);
    const RECT progress{bar.left, bar.top, std::max(bar.left + 1, bar.left + (bar.right - bar.left) * 3 / 5), bar.bottom};
    const HBRUSH progress_fill = CreateSolidBrush(color_ref(panel.bar.fill_color)); FillRect(dc, &progress, progress_fill); DeleteObject(progress_fill);
    const HPEN border = CreatePen(PS_SOLID, selected == &panel ? 2 : 1,
                                  selected == &panel ? preview_colors.selection : color_ref(panel.bar.border_color));
    const HGDIOBJ previous = SelectObject(dc, border); SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom); SelectObject(dc, previous); DeleteObject(border);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color_ref(panel.bar.text_color));
    const auto name = to_wide(panel.name); RECT text = bar; text.left += 4; text.right -= 4;
    DrawTextW(dc, name.c_str(), static_cast<int>(name.size()), &text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  EndPaint(window, &paint);
}

bool EditorWindow::handle_preview_message(const HWND window, const UINT message,
                                          const WPARAM, const LPARAM lparam) {
  switch (message) {
  case WM_PAINT: paint_preview(window); return true;
  case WM_ERASEBKGND: return true;
  case WM_LBUTTONDOWN: {
    const auto *panel = selected_bar();
    if (panel == nullptr || panel->cooldown_ids.size() != 1) return true;
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const auto bar = bar_preview_rect(*panel, preview_canvas());
    if (PtInRect(&bar, point)) {
      dragging_bar_ = true;
      bar_drag_moved_ = false;
      bar_drag_start_ = point;
      bar_drag_offset_ = {point.x - (bar.left + bar.right) / 2,
                          point.y - (bar.top + bar.bottom) / 2};
      SetCapture(window);
    }
    return true;
  }
  case WM_MOUSEMOVE:
    if (dragging_bar_) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != bar_drag_start_.x || point.y != bar_drag_start_.y)
        bar_drag_moved_ = true;
      if (bar_drag_moved_) update_dragged_bar(point);
    }
    return dragging_bar_;
  case WM_LBUTTONUP:
    if (dragging_bar_) {
      const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (point.x != bar_drag_start_.x || point.y != bar_drag_start_.y)
        bar_drag_moved_ = true;
      if (bar_drag_moved_) update_dragged_bar(point);
      dragging_bar_ = false;
      bar_drag_moved_ = false;
      if (GetCapture() == window) ReleaseCapture();
    }
    return true;
  case WM_CAPTURECHANGED:
    dragging_bar_ = false;
    bar_drag_moved_ = false;
    return false;
  default: return false;
  }
}

void EditorWindow::update_dragged_bar(const POINT point) {
  auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto canvas = preview_canvas();
  const LONG center_x = point.x - bar_drag_offset_.x;
  const LONG center_y = point.y - bar_drag_offset_.y;
  panel->anchor_x = std::clamp(
      (center_x - canvas.left) /
          static_cast<double>(std::max(1L, canvas.right - canvas.left)),
      0.0, 1.0);
  panel->anchor_y = std::clamp(
      (center_y - canvas.top) /
          static_cast<double>(std::max(1L, canvas.bottom - canvas.top)),
      0.0, 1.0);
  dirty_ = true; set_status(L"Bar position updated — save to apply it to LayerD.");
  InvalidateRect(preview_, nullptr, TRUE);
}
void EditorWindow::open_options() {
  DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_OPTIONS), dialog_, &EditorWindow::options_dialog_proc,
                  reinterpret_cast<LPARAM>(this));
}

void EditorWindow::initialize_options(const HWND window) {
  set_dark_title_bar(window); style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  set_owner_draw_checkbox(GetDlgItem(window, IDC_CAPTURE_PRIVACY),
                          capture_exclusion_);
  const HWND preset = GetDlgItem(window, IDC_THEME_PRESET);
  for (const wchar_t *name : {L"Graphite / Copper", L"Graphite / Teal", L"Graphite / Plum"})
    SendMessageW(preset, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
  SendMessageW(preset, LB_SETCURSEL, static_cast<WPARAM>(theme_), 0);
}

void EditorWindow::apply_options(const HWND window) {
  const bool capture =
      owner_draw_checkbox_checked(GetDlgItem(window, IDC_CAPTURE_PRIVACY));
  const auto selection = static_cast<int>(SendMessageW(GetDlgItem(window, IDC_THEME_PRESET), LB_GETCURSEL, 0, 0));
  const auto theme = static_cast<layerc::platform::EditorTheme>(std::clamp(selection, 0, 2));
  if (!layerc::platform::set_capture_exclusion_enabled(capture) ||
      !layerc::platform::set_editor_theme(theme)) {
    MessageBoxW(window, L"Could not save the selected options.", L"LayerE", MB_OK | MB_ICONWARNING);
    return;
  }
  capture_exclusion_ = capture; theme_ = theme; accent_ = layerc::platform::editor_accent();
  (void)layerc::platform::apply_capture_exclusion(dialog_, capture_exclusion_);
  if (task_dialog_ != nullptr) (void)layerc::platform::apply_capture_exclusion(task_dialog_, capture_exclusion_);
  (void)layerc::platform::signal_settings_reload();
  refresh_theme(); set_status(L"Options saved."); EndDialog(window, IDOK);
}

layerc::platform::EditorControlRole EditorWindow::appearance_role(const HWND window) const {
  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  const LRESULT row = SendMessageW(tree, LB_GETCURSEL, 0, 0);
  const LRESULT item = row == LB_ERR ? 0 : SendMessageW(tree, LB_GETITEMDATA, static_cast<WPARAM>(row), 0);
  return item == LB_ERR ? layerc::platform::EditorControlRole::window
                        : static_cast<layerc::platform::EditorControlRole>(item);
}

void EditorWindow::initialize_appearance(const HWND window) {
  set_dark_title_bar(window); style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  const std::array<std::pair<layerc::platform::EditorControlRole, const wchar_t *>, 10> roles{{
      {layerc::platform::EditorControlRole::window, L"Window"},
      {layerc::platform::EditorControlRole::toolbar, L"  Toolbar"},
      {layerc::platform::EditorControlRole::sidebar, L"  Saved Profiles"},
      {layerc::platform::EditorControlRole::label, L"  Labels"},
      {layerc::platform::EditorControlRole::button, L"  Buttons"},
      {layerc::platform::EditorControlRole::edit, L"  Text inputs"},
      {layerc::platform::EditorControlRole::list, L"  Lists"},
      {layerc::platform::EditorControlRole::checkbox, L"  Checkboxes"},
      {layerc::platform::EditorControlRole::preview, L"  Preview"},
      {layerc::platform::EditorControlRole::status, L"  Status"},
  }};
  for (const auto &[role, label] : roles) {
    const auto row = SendMessageW(tree, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(role));
  }
  SendMessageW(tree, LB_SETCURSEL, 0, 0);
  const HWND preview = GetDlgItem(window, IDC_APPEARANCE_PREVIEW);
  const auto style = GetWindowLongPtrW(preview, GWL_STYLE);
  SetWindowLongPtrW(preview, GWL_STYLE, (style & ~static_cast<LONG_PTR>(SS_TYPEMASK)) | SS_OWNERDRAW);
  refresh_appearance_controls(window);
}

std::uint32_t *EditorWindow::appearance_value(layerc::platform::EditorAppearanceOverrides &overrides,
                                               const std::uint32_t field) {
  switch (field) {
  case layerc::platform::editor_fill: return &overrides.fill;
  case layerc::platform::editor_text: return &overrides.text;
  case layerc::platform::editor_border: return &overrides.border;
  case layerc::platform::editor_hover: return &overrides.hover;
  case layerc::platform::editor_selection: return &overrides.selection;
  case layerc::platform::editor_disabled: return &overrides.disabled;
  default: return nullptr;
  }
}

COLORREF EditorWindow::appearance_color(const HWND window, const std::uint32_t field) const {
  const auto role = appearance_role(window);
  auto overrides = layerc::platform::editor_appearance_overrides(role);
  if (const auto *value = appearance_value(overrides, field);
      value != nullptr && (overrides.fields & field) != 0) return color_ref(*value);
  const auto role_colors = colors(role);
  switch (field) {
  case layerc::platform::editor_fill: return role_colors.fill;
  case layerc::platform::editor_text: return role_colors.text;
  case layerc::platform::editor_border: return role_colors.border;
  case layerc::platform::editor_hover: return role_colors.hover;
  case layerc::platform::editor_selection: return role_colors.selection;
  case layerc::platform::editor_disabled: return role_colors.disabled;
  default: return role_colors.fill;
  }
}

void EditorWindow::refresh_appearance_controls(const HWND window) {
  for (const int id : {IDC_APPEARANCE_FILL, IDC_APPEARANCE_TEXT, IDC_APPEARANCE_BORDER,
                       IDC_APPEARANCE_HOVER, IDC_APPEARANCE_SELECTION, IDC_APPEARANCE_DISABLED,
                       IDC_APPEARANCE_RESET, IDC_APPEARANCE_PREVIEW})
    InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
}

void EditorWindow::choose_appearance_color(const HWND window, const std::uint32_t field) {
  const auto role = appearance_role(window);
  auto overrides = layerc::platform::editor_appearance_overrides(role);
  CHOOSECOLORW choose{};
  choose.lStructSize = sizeof(choose); choose.hwndOwner = window;
  choose.rgbResult = appearance_color(window, field); choose.lpCustColors = custom_colors_.data();
  choose.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (ChooseColorW(&choose) == FALSE) return;
  if (auto *value = appearance_value(overrides, field); value != nullptr) {
    *value = rgb_value(choose.rgbResult); overrides.fields |= field;
    if (!layerc::platform::set_editor_appearance_overrides(role, overrides)) {
      MessageBoxW(window, L"Could not save the appearance override.", L"LayerE", MB_OK | MB_ICONWARNING);
      return;
    }
  }
  refresh_theme(); refresh_appearance_controls(window);
}

void EditorWindow::reset_appearance(const HWND window) {
  constexpr std::uint32_t all_fields = layerc::platform::editor_fill | layerc::platform::editor_text |
      layerc::platform::editor_border | layerc::platform::editor_hover |
      layerc::platform::editor_selection | layerc::platform::editor_disabled;
  if (!layerc::platform::reset_editor_appearance_overrides(appearance_role(window), all_fields)) {
    MessageBoxW(window, L"Could not reset this appearance override.", L"LayerE", MB_OK | MB_ICONWARNING);
    return;
  }
  refresh_theme(); refresh_appearance_controls(window);
}

void EditorWindow::draw_button(const DRAWITEMSTRUCT &draw,
                               const layerc::platform::EditorControlRole role,
                               const std::optional<COLORREF> explicit_fill) const {
  const auto role_colors = colors(role);
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
  const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
  const COLORREF fill = disabled ? role_colors.disabled
                        : pressed ? role_colors.selection
                        : hot ? role_colors.hover : role_colors.fill;
  const HBRUSH background = CreateSolidBrush(fill);
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  const HPEN pen = CreatePen(PS_SOLID, pressed ? 2 : 1,
                             pressed ? role_colors.selection : role_colors.border);
  const HGDIOBJ previous_pen = SelectObject(draw.hDC, pen);
  const HGDIOBJ previous_brush = SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
  Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top,
            draw.rcItem.right, draw.rcItem.bottom);
  SelectObject(draw.hDC, previous_brush);
  SelectObject(draw.hDC, previous_pen);
  DeleteObject(pen);

  RECT text_rect = draw.rcItem;
  UINT text_format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  if (explicit_fill.has_value()) {
    const int item_height = static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
    const int swatch_size = std::min(14, std::max(8, item_height - 10));
    const int swatch_top = draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - swatch_size) / 2;
    RECT swatch{draw.rcItem.left + 7, swatch_top,
                draw.rcItem.left + 7 + swatch_size, swatch_top + swatch_size};
    const HBRUSH swatch_fill = CreateSolidBrush(*explicit_fill);
    FillRect(draw.hDC, &swatch, swatch_fill);
    DeleteObject(swatch_fill);
    const HBRUSH swatch_border = CreateSolidBrush(role_colors.border);
    FrameRect(draw.hDC, &swatch, swatch_border);
    DeleteObject(swatch_border);
    text_rect.left = swatch.right + 7;
    text_rect.right -= 5;
    text_format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  }

  std::array<wchar_t, 160> label{};
  GetWindowTextW(draw.hwndItem, label.data(), static_cast<int>(label.size()));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, disabled ? role_colors.disabled : role_colors.text);
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
  DrawTextW(draw.hDC, label.data(), -1, &text_rect, text_format);
  SelectObject(draw.hDC, previous_font);
}
void EditorWindow::draw_list_box_item(const DRAWITEMSTRUCT &draw) const {
  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  const bool selected = (draw.itemState & ODS_SELECTED) != 0;
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const HBRUSH background = CreateSolidBrush(selected ? list_colors.selection
                                                       : list_colors.fill);
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  if (draw.itemID != static_cast<UINT>(-1)) {
    const LRESULT length = SendMessageW(draw.hwndItem, LB_GETTEXTLEN,
                                        draw.itemID, 0);
    if (length != LB_ERR) {
      std::wstring label(static_cast<std::size_t>(length) + 1U, L'\0');
      SendMessageW(draw.hwndItem, LB_GETTEXT, draw.itemID,
                   reinterpret_cast<LPARAM>(label.data()));
      label.resize(static_cast<std::size_t>(length));
      RECT text_rect = draw.rcItem;
      text_rect.left += 5;
      text_rect.right -= 4;
      SetBkMode(draw.hDC, TRANSPARENT);
      SetTextColor(draw.hDC, disabled ? list_colors.disabled : list_colors.text);
      const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
      DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()), &text_rect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(draw.hDC, previous_font);
    }
  }
  if ((draw.itemState & ODS_FOCUS) != 0) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -1, -1);
    const HBRUSH focus_brush = CreateSolidBrush(list_colors.border);
    FrameRect(draw.hDC, &focus, focus_brush);
    DeleteObject(focus_brush);
  }
}
void EditorWindow::draw_checkbox(const DRAWITEMSTRUCT &draw) const {
  const auto role_colors = colors(layerc::platform::EditorControlRole::checkbox);
  const bool checked = owner_draw_checkbox_checked(draw.hwndItem);
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const HBRUSH background = CreateSolidBrush(role_colors.fill); FillRect(draw.hDC, &draw.rcItem, background); DeleteObject(background);
  RECT box{draw.rcItem.left + 4, draw.rcItem.top + 4, draw.rcItem.left + 20, draw.rcItem.top + 20};
  const HBRUSH box_fill = CreateSolidBrush(checked ? role_colors.selection : role_colors.hover);
  FillRect(draw.hDC, &box, box_fill); DeleteObject(box_fill);
  const HPEN border = CreatePen(PS_SOLID, 1, role_colors.border); const auto previous = SelectObject(draw.hDC, border);
  SelectObject(draw.hDC, GetStockObject(NULL_BRUSH)); Rectangle(draw.hDC, box.left, box.top, box.right, box.bottom);
  if (checked) {
    const HPEN tick = CreatePen(PS_SOLID, 2, role_colors.text); SelectObject(draw.hDC, tick);
    MoveToEx(draw.hDC, box.left + 3, box.top + 8, nullptr); LineTo(draw.hDC, box.left + 7, box.bottom - 4);
    LineTo(draw.hDC, box.right - 3, box.top + 4); SelectObject(draw.hDC, border); DeleteObject(tick);
  }
  SelectObject(draw.hDC, previous); DeleteObject(border);
  std::array<wchar_t, 160> label{}; GetWindowTextW(draw.hwndItem, label.data(), static_cast<int>(label.size()));
  RECT text = draw.rcItem; text.left += 27; SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, disabled ? role_colors.disabled : role_colors.text);
  DrawTextW(draw.hDC, label.data(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void EditorWindow::draw_appearance_preview(const DRAWITEMSTRUCT &draw, const HWND window) const {
  const auto role_colors = colors(appearance_role(window));
  const HBRUSH background = CreateSolidBrush(role_colors.fill); FillRect(draw.hDC, &draw.rcItem, background); DeleteObject(background);
  RECT button{draw.rcItem.left + 16, draw.rcItem.top + 16, draw.rcItem.left + 145, draw.rcItem.top + 48};
  const HBRUSH button_fill = CreateSolidBrush(role_colors.hover); FillRect(draw.hDC, &button, button_fill); DeleteObject(button_fill);
  const HPEN border = CreatePen(PS_SOLID, 1, role_colors.border); const HGDIOBJ previous = SelectObject(draw.hDC, border);
  SelectObject(draw.hDC, GetStockObject(NULL_BRUSH)); Rectangle(draw.hDC, button.left, button.top, button.right, button.bottom);
  SelectObject(draw.hDC, previous); DeleteObject(border); SetBkMode(draw.hDC, TRANSPARENT); SetTextColor(draw.hDC, role_colors.text);
  DrawTextW(draw.hDC, L"Preview control", -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  RECT selected{button.right + 24, button.top, button.right + 112, button.bottom};
  const HBRUSH selection = CreateSolidBrush(role_colors.selection);
  FillRect(draw.hDC, &selected, selection);
  DeleteObject(selection);
  SetTextColor(draw.hDC, role_colors.text);
  DrawTextW(draw.hDC, L"Selected", -1, &selected,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  RECT disabled{button.left, button.bottom + 14, selected.right, button.bottom + 40};
  SetTextColor(draw.hDC, role_colors.disabled);
  DrawTextW(draw.hDC, L"Disabled text", -1, &disabled,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

LRESULT EditorWindow::color_message(const UINT message, const WPARAM wparam, const LPARAM lparam) const {
  const HWND control = reinterpret_cast<HWND>(lparam); const HDC dc = reinterpret_cast<HDC>(wparam);
  auto role = layerc::platform::EditorControlRole::label;
  if (message == WM_CTLCOLOREDIT) role = layerc::platform::EditorControlRole::edit;
  else if (message == WM_CTLCOLORLISTBOX) role = layerc::platform::EditorControlRole::list;
  else if (control == status_) role = layerc::platform::EditorControlRole::status;
  const auto role_colors = colors(role); SetTextColor(dc, role_colors.text); SetBkColor(dc, role_colors.fill);
  return reinterpret_cast<LRESULT>(brush(role));
}
LRESULT EditorWindow::handle_main_message(const HWND window, const UINT message,
                                          const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_SIZE: layout(); return TRUE;
  case WM_GETMINMAXINFO:
    reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {1080, 700}; return TRUE;
  case WM_COMMAND: {
    const int id = LOWORD(wparam); const auto notification = HIWORD(wparam);
    if (id == IDC_PROFILE_NAME || id == IDC_PROFILE_ID || id == IDC_BAR_WIDTH || id == IDC_BAR_HEIGHT) {
      if (!syncing_ && notification == EN_CHANGE) {
        sync_profile_from_controls();
        dirty_ = true;
        InvalidateRect(preview_, nullptr, TRUE);
      }
      return TRUE;
    }
    if (id == IDC_HIDE_READY && notification == BN_CLICKED) {
      toggle_owner_draw_checkbox(GetDlgItem(dialog_, IDC_HIDE_READY));
      sync_profile_from_controls();
      dirty_ = true; InvalidateRect(GetDlgItem(dialog_, IDC_HIDE_READY), nullptr, TRUE); return TRUE;
    }
    switch (id) {
    case IDC_FILE_MENU: show_file_menu(); return TRUE;
    case IDC_NEW_PROFILE: case IDM_FILE_NEW: new_profile(); return TRUE;
    case IDC_SAVE_PROFILE: case IDM_FILE_SAVE: (void)save_profile(false); return TRUE;
    case IDM_FILE_SAVE_AS: (void)save_profile(true); return TRUE;
    case IDC_ATTACH: case IDC_ATTACH_LEFT: attach_profile(); return TRUE;
    case IDC_SHOW_TASKS: case IDC_SHOW_TASKS_LEFT: show_tasks(); return TRUE;
    case IDC_OPTIONS: open_options(); return TRUE;
    case IDC_SPLIT_BARS: split_independent_bars(); return TRUE;
    case IDC_ADD_COOLDOWN: add_cooldown(); return TRUE;
    case IDC_REMOVE_COOLDOWN: remove_cooldown(); return TRUE;
    case IDC_CAPTURE_BINDING: begin_binding_capture(); return TRUE;
    case IDC_BAR_FILL: case IDC_BAR_BACKGROUND: case IDC_BAR_BORDER: case IDC_BAR_TEXT: choose_bar_color(id); return TRUE;
    case IDM_FILE_DELETE: delete_profile(); return TRUE;
    case IDM_FILE_OPEN_FOLDER: open_profiles_folder(); return TRUE;
    case IDM_FILE_EXIT: SendMessageW(dialog_, WM_CLOSE, 0, 0); return TRUE;
    default: break;
    }
    if (id == IDC_COOLDOWN_LIST && notification == LBN_SELCHANGE) {
      cancel_binding_capture();
      refresh_binding_controls();
      return TRUE;
    }
    if (id == IDC_BAR_LIST && notification == LBN_SELCHANGE) {
      sync_profile_from_controls();
      refresh_bar_controls();
      return TRUE;
    }
    break;
  }
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_PROFILE_LIST) {
      if (header->code == NM_CUSTOMDRAW) {
        auto *draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          const auto index = static_cast<std::size_t>(draw->nmcd.dwItemSpec);
          const auto list_colors = colors(layerc::platform::EditorControlRole::list);
          const bool enabled = index < entries_.size() && entries_[index].enabled;
          draw->clrText = enabled ? list_colors.text : list_colors.disabled;
          draw->clrTextBk = (draw->nmcd.uItemState & CDIS_SELECTED) != 0 ? list_colors.selection : list_colors.fill;
          return CDRF_NEWFONT;
        }
      } else if (header->code == LVN_ITEMCHANGED) {
        const auto *change = reinterpret_cast<const NMLISTVIEW *>(lparam);
        if (syncing_) return TRUE;
        if ((change->uChanged & LVIF_STATE) != 0 &&
            ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) != 0) {
          save_profile_enabled(change->iItem, ListView_GetCheckState(profile_list_, change->iItem) != FALSE);
          return TRUE;
        }
        if ((change->uChanged & LVIF_STATE) != 0 && (change->uNewState & LVIS_SELECTED) != 0 &&
            (change->uOldState & LVIS_SELECTED) == 0) { open_profile_row(change->iItem, true); return TRUE; }
      }
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTBOX) { draw_list_box_item(*draw); return TRUE; }
    if (draw->CtlType != ODT_BUTTON) break;
    const int id = static_cast<int>(draw->CtlID);
    if (id == IDC_HIDE_READY) { draw_checkbox(*draw); return TRUE; }
    std::optional<COLORREF> overlay_color;
    if (const auto *panel = selected_bar()) {
      if (id == IDC_BAR_FILL) overlay_color = color_ref(panel->bar.fill_color);
      else if (id == IDC_BAR_BACKGROUND) overlay_color = color_ref(panel->bar.background_color);
      else if (id == IDC_BAR_BORDER) overlay_color = color_ref(panel->bar.border_color);
      else if (id == IDC_BAR_TEXT) overlay_color = color_ref(panel->bar.text_color);
    }
    const bool toolbar = id == IDC_FILE_MENU || id == IDC_NEW_PROFILE || id == IDC_SAVE_PROFILE || id == IDC_ATTACH ||
                         id == IDC_SHOW_TASKS || id == IDC_SPLIT_BARS || id == IDC_OPTIONS;
    draw_button(*draw, toolbar ? layerc::platform::EditorControlRole::toolbar
                               : layerc::platform::EditorControlRole::button, overlay_color);
    return TRUE;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: {
    RECT client{}; GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client, brush(layerc::platform::EditorControlRole::window)); return TRUE;
  }
  case WM_LBUTTONUP:
    if (picking_) { finish_picker(); return TRUE; }
    break;
  case WM_KEYDOWN:
    if (picking_ && wparam == VK_ESCAPE) { cancel_picker(); set_status(L"Window picker cancelled."); return TRUE; }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
      if (wparam == 'S') { (void)save_profile(false); return TRUE; }
      if (wparam == 'N') { new_profile(); return TRUE; }
    }
    break;
  case WM_CAPTURECHANGED:
    if (picking_) cancel_picker();
    break;
  case WM_CLOSE:
    if (confirm_unsaved_changes()) DestroyWindow(window);
    return TRUE;
  case WM_DESTROY:
    cancel_picker();
    if (task_dialog_ != nullptr && IsWindow(task_dialog_)) DestroyWindow(task_dialog_);
    task_dialog_ = nullptr;
    if (profile_check_images_ != nullptr) {
      ImageList_Destroy(profile_check_images_);
      profile_check_images_ = nullptr;
    }
    for (auto &brush_value : brushes_) { if (brush_value != nullptr) DeleteObject(brush_value); brush_value = nullptr; }
    if (font_ != nullptr) { DeleteObject(font_); font_ = nullptr; }
    PostQuitMessage(0); return TRUE;
  default: break;
  }
  return FALSE;
}

LRESULT EditorWindow::handle_task_message(const HWND window, const UINT message,
                                          const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND:
    switch (LOWORD(wparam)) {
    case IDC_PICK_WINDOW: begin_picker(); return TRUE;
    case IDC_USE_TASK: use_selected_task(); return TRUE;
    case IDC_REFRESH_TASKS: refresh_tasks(); return TRUE;
    default: break;
    }
    break;
  case WM_NOTIFY: {
    const auto *header = reinterpret_cast<const NMHDR *>(lparam);
    if (header->idFrom == IDC_TASK_LIST) {
      if (header->code == LVN_ITEMCHANGED) update_task_detail();
      else if (header->code == NM_DBLCLK) use_selected_task();
      else if (header->code == NM_CUSTOMDRAW) {
        auto *draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lparam);
        const auto list_colors = colors(layerc::platform::EditorControlRole::list);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          draw->clrText = list_colors.text;
          draw->clrTextBk = (draw->nmcd.uItemState & CDIS_SELECTED) != 0 ? list_colors.selection : list_colors.fill;
          return CDRF_NEWFONT;
        }
      }
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_BUTTON) { draw_button(*draw, layerc::platform::EditorControlRole::button); return TRUE; }
    break;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: { RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wparam), &client,
    brush(layerc::platform::EditorControlRole::window)); return TRUE; }
  case WM_CLOSE: ShowWindow(window, SW_HIDE); return TRUE;
  case WM_DESTROY:
    if (task_dialog_ == window) { task_dialog_ = nullptr; task_list_ = nullptr; }
    return TRUE;
  default: break;
  }
  return FALSE;
}

LRESULT EditorWindow::handle_options_message(const HWND window, const UINT message,
                                             const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND:
    if (LOWORD(wparam) == IDC_CAPTURE_PRIVACY && HIWORD(wparam) == BN_CLICKED) {
      toggle_owner_draw_checkbox(GetDlgItem(window, IDC_CAPTURE_PRIVACY));
      InvalidateRect(GetDlgItem(window, IDC_CAPTURE_PRIVACY), nullptr, TRUE); return TRUE;
    }
    if (LOWORD(wparam) == IDC_OPEN_APPEARANCE) {
      DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_APPEARANCE), window, &EditorWindow::appearance_dialog_proc,
                      reinterpret_cast<LPARAM>(this));
      refresh_theme(); return TRUE;
    }
    if (LOWORD(wparam) == IDC_OPTIONS_APPLY) { apply_options(window); return TRUE; }
    if (LOWORD(wparam) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    break;
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTBOX) { draw_list_box_item(*draw); return TRUE; }
    if (draw->CtlType == ODT_BUTTON) {
      if (draw->CtlID == IDC_CAPTURE_PRIVACY) draw_checkbox(*draw);
      else draw_button(*draw, layerc::platform::EditorControlRole::button);
      return TRUE;
    }
    break;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: { RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wparam), &client,
    brush(layerc::platform::EditorControlRole::window)); return TRUE; }
  default: break;
  }
  return FALSE;
}

LRESULT EditorWindow::handle_appearance_message(const HWND window, const UINT message,
                                                const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND: {
    const int id = LOWORD(wparam);
    if (id == IDC_APPEARANCE_TREE && HIWORD(wparam) == LBN_SELCHANGE) { refresh_appearance_controls(window); return TRUE; }
    constexpr std::array<std::pair<int, std::uint32_t>, 6> color_actions{{
        {IDC_APPEARANCE_FILL, layerc::platform::editor_fill}, {IDC_APPEARANCE_TEXT, layerc::platform::editor_text},
        {IDC_APPEARANCE_BORDER, layerc::platform::editor_border}, {IDC_APPEARANCE_HOVER, layerc::platform::editor_hover},
        {IDC_APPEARANCE_SELECTION, layerc::platform::editor_selection}, {IDC_APPEARANCE_DISABLED, layerc::platform::editor_disabled},
    }};
    for (const auto &[button, field] : color_actions) if (id == button) { choose_appearance_color(window, field); return TRUE; }
    if (id == IDC_APPEARANCE_RESET) { reset_appearance(window); return TRUE; }
    if (id == IDOK || id == IDCANCEL) { EndDialog(window, id); return TRUE; }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTBOX) { draw_list_box_item(*draw); return TRUE; }
    if (draw->CtlID == IDC_APPEARANCE_PREVIEW) { draw_appearance_preview(*draw, window); return TRUE; }
    if (draw->CtlType == ODT_BUTTON) {
      std::optional<COLORREF> fill;
      constexpr std::array<std::pair<int, std::uint32_t>, 6> color_actions{{
          {IDC_APPEARANCE_FILL, layerc::platform::editor_fill}, {IDC_APPEARANCE_TEXT, layerc::platform::editor_text},
          {IDC_APPEARANCE_BORDER, layerc::platform::editor_border}, {IDC_APPEARANCE_HOVER, layerc::platform::editor_hover},
          {IDC_APPEARANCE_SELECTION, layerc::platform::editor_selection}, {IDC_APPEARANCE_DISABLED, layerc::platform::editor_disabled},
      }};
      for (const auto &[button, field] : color_actions) if (static_cast<int>(draw->CtlID) == button) fill = appearance_color(window, field);
      draw_button(*draw, layerc::platform::EditorControlRole::button, fill); return TRUE;
    }
    break;
  }
  case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: { RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wparam), &client,
    brush(layerc::platform::EditorControlRole::window)); return TRUE; }
  default: break;
  }
  return FALSE;
}

int EditorApp::run(const HINSTANCE instance, const int show_command) {
  EditorWindow editor;
  return editor.run(instance, show_command);
}

} // namespace layerc::editor