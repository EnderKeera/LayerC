#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

#include "layerc/platform/platform_events.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <windows.h>
namespace layerc::editor {
namespace {

struct AppearanceFieldAction {
  int control{};
  std::uint32_t field{};
  std::uint32_t layerc::platform::EditorAppearanceOverrides::*override_value{};
  COLORREF RoleColors::*resolved_value{};
};

struct AppearancePreset {
  layerc::platform::EditorTheme theme{};
  const wchar_t *name{};
};

// Hover contrast only applies to interactive chrome; selection contrast is
// checked on list-like roles as well.
bool contrast_pair_applies(const layerc::platform::EditorControlRole role,
                           const int control) {
  using Role = layerc::platform::EditorControlRole;
  if (control == IDC_APPEARANCE_HOVER)
    return role == Role::button || role == Role::checkbox;
  if (control == IDC_APPEARANCE_SELECTION)
    return role == Role::button || role == Role::checkbox ||
           role == Role::list || role == Role::sidebar;
  return true;
}

std::wstring find_role_label(const HWND roles,
                             const layerc::platform::EditorControlRole role) {
  const auto count = SendMessageW(roles, LB_GETCOUNT, 0, 0);
  for (LRESULT row = 0; row < count; ++row) {
    if (SendMessageW(roles, LB_GETITEMDATA, row, 0) !=
        static_cast<LRESULT>(role))
      continue;
    SendMessageW(roles, LB_SETCURSEL, row, 0);
    const auto length = SendMessageW(roles, LB_GETTEXTLEN, row, 0);
    if (length == LB_ERR)
      return {};
    std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(roles, LB_GETTEXT, row,
                 reinterpret_cast<LPARAM>(name.data()));
    name.resize(static_cast<std::size_t>(length));
    return name;
  }
  return {};
}

constexpr std::array appearance_presets{
    AppearancePreset{layerc::platform::EditorTheme::system,
                     L"System accent (dark)"},
    AppearancePreset{layerc::platform::EditorTheme::dark, L"Dark"},
    AppearancePreset{layerc::platform::EditorTheme::copper,
                     L"Graphite / Copper"},
    AppearancePreset{layerc::platform::EditorTheme::teal,
                     L"Graphite / Teal"},
    AppearancePreset{layerc::platform::EditorTheme::plum,
                     L"Graphite / Plum"},
};

void select_appearance_preset(const HWND window,
                              const layerc::platform::EditorTheme theme) {
  const auto found =
      std::ranges::find(appearance_presets, theme, &AppearancePreset::theme);
  if (found == appearance_presets.end()) return;
  SendDlgItemMessageW(
      window, IDC_THEME_PRESET, CB_SETCURSEL,
      static_cast<WPARAM>(found - appearance_presets.begin()), 0);
}

constexpr std::array appearance_fields{
    AppearanceFieldAction{IDC_APPEARANCE_FILL, layerc::platform::editor_fill,
                          &layerc::platform::EditorAppearanceOverrides::fill,
                          &RoleColors::fill},
    AppearanceFieldAction{IDC_APPEARANCE_TEXT, layerc::platform::editor_text,
                          &layerc::platform::EditorAppearanceOverrides::text,
                          &RoleColors::text},
    AppearanceFieldAction{IDC_APPEARANCE_BORDER,
                          layerc::platform::editor_border,
                          &layerc::platform::EditorAppearanceOverrides::border,
                          &RoleColors::border},
    AppearanceFieldAction{IDC_APPEARANCE_HOVER, layerc::platform::editor_hover,
                          &layerc::platform::EditorAppearanceOverrides::hover,
                          &RoleColors::hover},
    AppearanceFieldAction{IDC_APPEARANCE_SELECTION,
                          layerc::platform::editor_selection,
                          &layerc::platform::EditorAppearanceOverrides::selection,
                          &RoleColors::selection},
    AppearanceFieldAction{IDC_APPEARANCE_DISABLED,
                          layerc::platform::editor_disabled,
                          &layerc::platform::EditorAppearanceOverrides::disabled,
                          &RoleColors::disabled},
};

constexpr std::array appearance_advanced_controls{
    IDC_APPEARANCE_ROLE_LABEL,     IDC_APPEARANCE_TREE,
    IDC_APPEARANCE_OVERRIDE_LABEL, IDC_APPEARANCE_FILL,
    IDC_APPEARANCE_TEXT,           IDC_APPEARANCE_BORDER,
    IDC_APPEARANCE_HOVER,          IDC_APPEARANCE_SELECTION,
    IDC_APPEARANCE_DISABLED,       IDC_APPEARANCE_RESET,
};

constexpr std::uint32_t appearance_all_fields = [] {
  std::uint32_t result{};
  for (const auto &entry : appearance_fields)
    result |= entry.field;
  return result;
}();

const AppearanceFieldAction *appearance_field(const std::uint32_t field) {
  const auto found = std::ranges::find(appearance_fields, field,
                                       &AppearanceFieldAction::field);
  return found == appearance_fields.end() ? nullptr : &*found;
}

const AppearanceFieldAction *appearance_field_for_control(const int control) {
  const auto found = std::ranges::find(appearance_fields, control,
                                       &AppearanceFieldAction::control);
  return found == appearance_fields.end() ? nullptr : &*found;
}

constexpr std::array appearance_footer_controls{
    IDC_APPEARANCE_RESET_ALL, IDC_APPEARANCE_APPLY, IDOK, IDCANCEL};

// Advanced colours are progressive disclosure, not a saved setting: the
// button shows or hides the per-role overrides and the dialog grows or
// shrinks by exactly their band, so no empty space is left behind.
void show_advanced_appearance(const HWND window, const bool visible) {
  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  const bool shown = (GetWindowLongPtrW(tree, GWL_STYLE) & WS_VISIBLE) != 0;
  // Measured once while the template still shows the band: the distance
  // from the role label down to the footer row.
  constexpr wchar_t band_property[] = L"LayerC.AdvancedBand";
  if (GetPropW(window, band_property) == nullptr && shown) {
    RECT band_top{};
    RECT footer{};
    GetWindowRect(GetDlgItem(window, IDC_APPEARANCE_ROLE_LABEL), &band_top);
    GetWindowRect(GetDlgItem(window, IDC_APPEARANCE_RESET_ALL), &footer);
    SetPropW(window, band_property,
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(
                 std::max(0L, footer.top - band_top.top))));
  }
  const int band = static_cast<int>(
      reinterpret_cast<INT_PTR>(GetPropW(window, band_property)));
  if (shown != visible && band > 0) {
    const int delta = visible ? band : -band;
    for (const int id : appearance_footer_controls) {
      const HWND control = GetDlgItem(window, id);
      RECT bounds{};
      GetWindowRect(control, &bounds);
      MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT *>(&bounds),
                      2);
      SetWindowPos(control, nullptr, bounds.left, bounds.top + delta, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT frame{};
    GetWindowRect(window, &frame);
    SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left,
                 frame.bottom - frame.top + delta,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  for (const int id : appearance_advanced_controls)
    ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE);
  set_text(GetDlgItem(window, IDC_APPEARANCE_ADVANCED),
           visible ? L"Hide &advanced colors" : L"Show &advanced colors");
  InvalidateRect(window, nullptr, TRUE);
}

double relative_luminance(const COLORREF color) {
  const auto channel = [](const BYTE value) {
    const double component = static_cast<double>(value) / 255.0;
    return component <= 0.04045
               ? component / 12.92
               : std::pow((component + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(GetRValue(color)) +
         0.7152 * channel(GetGValue(color)) +
         0.0722 * channel(GetBValue(color));
}

double contrast_ratio(const COLORREF first, const COLORREF second) {
  const double first_luminance = relative_luminance(first);
  const double second_luminance = relative_luminance(second);
  return (std::max(first_luminance, second_luminance) + 0.05) /
         (std::min(first_luminance, second_luminance) + 0.05);
}

} // namespace

void EditorWindow::open_options() {
  DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_OPTIONS), shell_.window,
      &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_options,
                              &EditorWindow::handle_options_message>,
      reinterpret_cast<LPARAM>(this));
}

void EditorWindow::open_appearance() {
  const auto loaded = layerc::platform::read_editor_appearance();
  if (loaded.failed()) {
    themed_message_box(shell_.window, L"Could not read the editor appearance settings.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return;
  }
  appearance_editor_.draft().begin(loaded.value.value_or(
      layerc::platform::EditorAppearanceSettings{}));
  appearance_editor_.begin_open();
  refresh_theme();
  DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_APPEARANCE), shell_.window,
      &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_appearance,
                              &EditorWindow::handle_appearance_message>,
      reinterpret_cast<LPARAM>(this));
  appearance_editor_.close();
  appearance_editor_.draft().cancel();
  appearance_editor_.set_appearance(appearance_editor_.draft().staged().theme,
                                    appearance_editor_.draft().staged().accent);
  refresh_theme();
}

void EditorWindow::initialize_options(const HWND window) {
  apply_dialog_chrome(window, shell_.capture_exclusion);
  style_dialog_children(window);
  const auto visual = layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::visual_recognition);
  if (visual.failed())
    themed_message_box(window, L"Could not read the visual-recognition setting.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
  CheckDlgButton(window, IDC_VISUAL_MASTER,
                 visual.value.value_or(visual.missing()) ? BST_CHECKED
                                                         : BST_UNCHECKED);
  const auto overlays = layerc::platform::read_bool_setting(
      layerc::platform::BoolSetting::capture_exclusion);
  CheckDlgButton(window, IDC_CAPTURE_PRIVACY,
                 overlays.value.value_or(true) ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(window, IDC_EDITOR_CAPTURE_PRIVACY,
                 shell_.capture_exclusion ? BST_CHECKED : BST_UNCHECKED);
  const HWND text_size = GetDlgItem(window, IDC_TEXT_SIZE);
  for (const int percent : text_scale_choices) {
    const auto label = std::to_wstring(percent) + L" %";
    const LRESULT row = SendMessageW(text_size, CB_ADDSTRING, 0,
                                     reinterpret_cast<LPARAM>(label.c_str()));
    if (row >= 0) {
      SendMessageW(text_size, CB_SETITEMDATA, static_cast<WPARAM>(row), percent);
      if (percent == layerc::platform::native_text_scale())
        SendMessageW(text_size, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
    }
  }
  EnableWindow(GetDlgItem(window, IDC_OPTIONS_APPLY), FALSE);
}

bool EditorWindow::apply_options(const HWND window) {
  const bool overlays =
      IsDlgButtonChecked(window, IDC_CAPTURE_PRIVACY) == BST_CHECKED;
  const bool capture =
      IsDlgButtonChecked(window, IDC_EDITOR_CAPTURE_PRIVACY) == BST_CHECKED;
  const bool visual =
      IsDlgButtonChecked(window, IDC_VISUAL_MASTER) == BST_CHECKED;
  const LRESULT text_row =
      SendDlgItemMessageW(window, IDC_TEXT_SIZE, CB_GETCURSEL, 0, 0);
  const int text_scale =
      text_row >= 0 ? static_cast<int>(SendDlgItemMessageW(
                          window, IDC_TEXT_SIZE, CB_GETITEMDATA,
                          static_cast<WPARAM>(text_row), 0))
                    : layerc::platform::native_text_scale();
  bool saved = static_cast<bool>(
      layerc::platform::write_bool_setting(
          layerc::platform::BoolSetting::capture_exclusion, overlays));
  if (!layerc::platform::write_bool_setting(
          layerc::platform::BoolSetting::editor_capture_exclusion, capture))
    saved = false;
  if (!layerc::platform::write_bool_setting(
          layerc::platform::BoolSetting::visual_recognition, visual))
    saved = false;
  if (!layerc::platform::write_layerc_registry_dword(
          text_scale_setting, static_cast<std::uint32_t>(text_scale)))
    saved = false;
  if (!saved) {
    set_status(L"Could not save the selected options.");
    themed_message_box(window, L"Could not save the selected options.",
                L"Options", MB_OK | MB_ICONWARNING);
    return false;
  }

  EnableWindow(GetDlgItem(window, IDC_OPTIONS_APPLY), FALSE);
  if (text_scale != layerc::platform::native_text_scale()) {
    layerc::platform::set_native_text_scale(text_scale);
    reload_fonts();
  }
  shell_.capture_exclusion = capture;
  (void)layerc::platform::apply_capture_exclusion(window, shell_.capture_exclusion);
  (void)layerc::platform::apply_capture_exclusion(shell_.window, shell_.capture_exclusion);
  if (task_picker_.window != nullptr && IsWindow(task_picker_.window))
    (void)layerc::platform::apply_capture_exclusion(task_picker_.window,
                                                    shell_.capture_exclusion);
  (void)layerc::platform::signal_settings_reload();
  set_status(L"Options saved.");
  return true;
}

layerc::platform::EditorControlRole
EditorWindow::appearance_role(const HWND window) const {
  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  const LRESULT row = SendMessageW(tree, LB_GETCURSEL, 0, 0);
  const LRESULT item = row == LB_ERR
                           ? 0
                           : SendMessageW(tree, LB_GETITEMDATA,
                                          static_cast<WPARAM>(row), 0);
  if (item == LB_ERR || item < 0 ||
      item >= static_cast<LRESULT>(layerc::platform::EditorControlRole::count))
    return layerc::platform::EditorControlRole::window;
  return static_cast<layerc::platform::EditorControlRole>(item);
}

void EditorWindow::initialize_appearance(const HWND window) {
  apply_dialog_chrome(window, shell_.capture_exclusion);
  style_dialog_children(window);

  const HWND preset = GetDlgItem(window, IDC_THEME_PRESET);
  for (const auto &entry : appearance_presets) {
    const LRESULT row = SendMessageW(
        preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name));
    if (row >= 0)
      SendMessageW(preset, CB_SETITEMDATA, static_cast<WPARAM>(row),
                   static_cast<LPARAM>(entry.theme));
  }
  select_appearance_preset(window, appearance_editor_.draft().staged().theme);

  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  const std::array<
      std::pair<layerc::platform::EditorControlRole, const wchar_t *>, 10>
      roles{{
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
    const auto row =
        SendMessageW(tree, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(row),
                 static_cast<LPARAM>(role));
  }
  SendMessageW(tree, LB_SETCURSEL, 0, 0);
  show_advanced_appearance(window, false);
  refresh_appearance_controls(window);
}

std::uint32_t *EditorWindow::appearance_value(
    layerc::platform::EditorAppearanceOverrides &overrides,
    const std::uint32_t field) {
  const auto *entry = appearance_field(field);
  return entry == nullptr ? nullptr : &(overrides.*(entry->override_value));
}

std::optional<COLORREF>
EditorWindow::appearance_color(const HWND window,
                               const std::uint32_t field) const {
  const auto *entry = appearance_field(field);
  if (entry == nullptr)
    return std::nullopt;
  const auto role = appearance_role(window);
  const auto &appearance = appearance_editor_.draft().staged();
  return layerc::platform::native_theme_colors(appearance, role)
      .*(entry->resolved_value);
}

void EditorWindow::refresh_appearance_controls(const HWND window) {
  RedrawWindow(window, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void EditorWindow::choose_appearance_color(const HWND window,
                                           const std::uint32_t field) {
  const auto role = appearance_role(window);
  const auto index = static_cast<std::size_t>(role);
  auto &appearance = appearance_editor_.draft().staged();
  const auto current = appearance_color(window, field);
  if (!current)
    return;
  std::uint32_t selected = native_rgb_value(*current);
  if (!choose_color(window, selected))
    return;
  if (auto *value = appearance_value(appearance.overrides[index], field);
      value != nullptr) {
    *value = selected;
    appearance.overrides[index].fields |= field;
  }
  refresh_theme();
  refresh_appearance_controls(window);
}

void EditorWindow::reset_appearance(const HWND window) {
  const auto index = static_cast<std::size_t>(appearance_role(window));
  auto &appearance = appearance_editor_.draft().staged();
  appearance.overrides[index].fields &= ~appearance_all_fields;
  refresh_theme();
  refresh_appearance_controls(window);
}

void EditorWindow::reset_all_appearance(const HWND window) {
  auto &appearance = appearance_editor_.draft().staged();
  appearance.theme = layerc::platform::EditorTheme::copper;
  for (auto &overrides : appearance.overrides)
    overrides = {};
  select_appearance_preset(window, appearance.theme);
  refresh_theme();
  refresh_appearance_controls(window);
}

bool EditorWindow::apply_appearance(const HWND window) {
  const auto &appearance = appearance_editor_.draft().staged();
  for (std::size_t index = 0; index < appearance.overrides.size(); ++index) {
    const auto role =
        static_cast<layerc::platform::EditorControlRole>(index);
    const auto role_colors =
        layerc::platform::native_theme_colors(appearance, role);
    struct ContrastPair {
      const wchar_t *name;
      COLORREF foreground;
      COLORREF background;
      double minimum;
      int control;
    };
    const std::array pairs{
        ContrastPair{L"Text / Fill", role_colors.text, role_colors.fill, 4.5, IDC_APPEARANCE_TEXT},
        ContrastPair{L"Disabled / Fill", role_colors.disabled, role_colors.fill, 3.0, IDC_APPEARANCE_DISABLED},
        ContrastPair{L"Text / Hover", role_colors.text, role_colors.hover, 4.5, IDC_APPEARANCE_HOVER},
        ContrastPair{L"Text / Selection", role_colors.text, role_colors.selection, 4.5, IDC_APPEARANCE_SELECTION}};
    for (const auto &pair : pairs) {
      if (!contrast_pair_applies(role, pair.control))
        continue;
      const auto ratio = contrast_ratio(pair.foreground, pair.background);
      if (ratio >= pair.minimum)
        continue;
      const HWND roles = GetDlgItem(window, IDC_APPEARANCE_TREE);
      const auto role_name = find_role_label(roles, role);
      refresh_appearance_controls(window);
      const auto message = role_name + L": " + pair.name + L" contrast is " +
          std::to_wstring(ratio).substr(0, 4) + L":1; at least " +
          std::to_wstring(pair.minimum).substr(0, 3) +
          L":1 is required. Adjust this override or use Reset selected.";
      themed_message_box(window, message.c_str(), L"LayerC Editor", MB_OK | MB_ICONWARNING);
      SetFocus(GetDlgItem(window, pair.control));
      return false;
    }
  }

  if (!appearance_editor_.draft().apply(layerc::platform::write_editor_appearance)) {
    themed_message_box(window, L"Could not save the selected appearance.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return false;
  }

  appearance_editor_.set_appearance(appearance.theme, appearance.accent);
  (void)layerc::platform::signal_settings_reload();
  refresh_theme();
  set_status(L"Appearance saved.");
  return true;
}

LRESULT EditorWindow::handle_options_message(const HWND window,
                                             const UINT message,
                                             const WPARAM wparam,
                                             const LPARAM lparam) {
  if (message == WM_COMMAND) {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if ((notification == BN_CLICKED &&
         (id == IDC_VISUAL_MASTER || id == IDC_CAPTURE_PRIVACY ||
          id == IDC_EDITOR_CAPTURE_PRIVACY)) ||
        (id == IDC_TEXT_SIZE && notification == CBN_SELCHANGE)) {
      EnableWindow(GetDlgItem(window, IDC_OPTIONS_APPLY), TRUE);
      return TRUE;
    }
    if (id == IDC_OPTIONS_APPLY && notification == BN_CLICKED) {
      (void)apply_options(window);
      return TRUE;
    }
    if (id == IDOK) {
      if (IsWindowEnabled(GetDlgItem(window, IDC_OPTIONS_APPLY)) == FALSE ||
          apply_options(window))
        EndDialog(window, IDOK);
      return TRUE;
    }
    if (id == IDCANCEL) {
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

LRESULT EditorWindow::handle_appearance_message(const HWND window,
                                                const UINT message,
                                                const WPARAM wparam,
                                                const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND: {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (id == IDC_THEME_PRESET && notification == CBN_SELCHANGE) {
      const LRESULT selection =
          SendDlgItemMessageW(window, IDC_THEME_PRESET, CB_GETCURSEL, 0, 0);
      if (selection != CB_ERR) {
        const LRESULT theme = SendDlgItemMessageW(
            window, IDC_THEME_PRESET, CB_GETITEMDATA,
            static_cast<WPARAM>(selection), 0);
        if (theme != CB_ERR)
          appearance_editor_.draft().staged().theme =
              static_cast<layerc::platform::EditorTheme>(theme);
      }
      refresh_theme();
      refresh_appearance_controls(window);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_ADVANCED && notification == BN_CLICKED) {
      show_advanced_appearance(
          window, (GetWindowLongPtrW(GetDlgItem(window, IDC_APPEARANCE_TREE),
                                     GWL_STYLE) &
                   WS_VISIBLE) == 0);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_TREE && notification == LBN_SELCHANGE) {
      refresh_appearance_controls(window);
      return TRUE;
    }
    if (const auto *entry = appearance_field_for_control(id);
        entry != nullptr && notification == BN_CLICKED) {
      choose_appearance_color(window, entry->field);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_RESET && notification == BN_CLICKED) {
      reset_appearance(window);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_RESET_ALL && notification == BN_CLICKED) {
      reset_all_appearance(window);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_APPLY && notification == BN_CLICKED) {
      (void)apply_appearance(window);
      return TRUE;
    }
    if (id == IDOK) {
      if (apply_appearance(window))
        EndDialog(window, IDOK);
      return TRUE;
    }
    if (id == IDCANCEL) {
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw->CtlType == ODT_LISTBOX) {
      draw_list_box_item(*draw);
      return TRUE;
    }
    if (draw->CtlID == IDC_APPEARANCE_PREVIEW) {
      draw_appearance_preview(*draw, window);
      return TRUE;
    }
    if (draw->CtlType == ODT_BUTTON) {
      std::optional<COLORREF> fill;
      if (const auto *entry =
              appearance_field_for_control(static_cast<int>(draw->CtlID)))
        fill = appearance_color(window, entry->field);
      draw_button(*draw, layerc::platform::EditorControlRole::button, fill);
      return TRUE;
    }
    break;
  }
  case WM_DESTROY:
    RemovePropW(window, L"LayerC.AdvancedBand");
    break;
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
