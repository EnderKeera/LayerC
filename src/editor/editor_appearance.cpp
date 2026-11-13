#include "editor_window.hpp"
#include "accessible_text.hpp"
#include "appearance_contrast.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_dialog_layouts.hpp"
#include "editor_resource.h"
#include "static_style.hpp"

#include "layerc/platform/platform_events.hpp"

#include <algorithm>
#include <array>
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

void select_role_row(const HWND roles,
                     const layerc::platform::EditorControlRole role) {
  const auto count = SendMessageW(roles, LB_GETCOUNT, 0, 0);
  for (LRESULT row = 0; row < count; ++row) {
    if (SendMessageW(roles, LB_GETITEMDATA, static_cast<WPARAM>(row), 0) ==
        static_cast<LRESULT>(role)) {
      SendMessageW(roles, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
      return;
    }
  }
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

bool advanced_appearance_shown(const HWND window) {
  return (GetWindowLongPtrW(GetDlgItem(window, IDC_APPEARANCE_TREE),
                            GWL_STYLE) &
          WS_VISIBLE) != 0;
}

// Advanced colors are progressive disclosure, not a saved setting: the
// button shows or hides the per-role overrides, and the code layout gives
// their band room only while it shows, so no empty space is left behind.
void show_advanced_appearance(const HWND window, const bool visible) {
  for (const int id : appearance_advanced_controls)
    ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE);
  set_text(GetDlgItem(window, IDC_APPEARANCE_ADVANCED),
           visible ? L"Hide &advanced colors" : L"Show &advanced colors");
  layout_appearance_dialog(window, visible);
  InvalidateRect(window, nullptr, TRUE);
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
  for (const int hint : {IDC_OPTIONS_VISUAL_HINT, IDC_OPTIONS_CAPTURE_HINT,
                         IDC_TEXT_SIZE_HINT, IDC_OPTIONS_UPDATES_HINT})
    set_text_tone(GetDlgItem(window, hint), TextTone::subtext);
  layout_options_dialog(window);
  center_over_owner(window);
  // Each hint line is the description of the control it explains
  // (DesignSystem.md, Screen readers).
  describe_with(GetDlgItem(window, IDC_VISUAL_MASTER),
                GetDlgItem(window, IDC_OPTIONS_VISUAL_HINT));
  for (const int id : {IDC_CAPTURE_PRIVACY, IDC_EDITOR_CAPTURE_PRIVACY})
    describe_with(GetDlgItem(window, id),
                  GetDlgItem(window, IDC_OPTIONS_CAPTURE_HINT));
  describe_with(GetDlgItem(window, IDC_TEXT_SIZE),
                GetDlgItem(window, IDC_TEXT_SIZE_HINT));
  describe_with(GetDlgItem(window, IDC_LAUNCHER_UPDATES),
                GetDlgItem(window, IDC_OPTIONS_UPDATES_HINT));
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
  // Like the launcher, a missing or unreadable preference reads as off.
  const auto updates = layerc::platform::read_bool_setting(
      layerc::platform::BoolSetting::launcher_updates);
  CheckDlgButton(window, IDC_LAUNCHER_UPDATES,
                 updates.value.value_or(false) ? BST_CHECKED : BST_UNCHECKED);
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
  const bool updates =
      IsDlgButtonChecked(window, IDC_LAUNCHER_UPDATES) == BST_CHECKED;
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
  if (!layerc::platform::write_bool_setting(
          layerc::platform::BoolSetting::launcher_updates, updates))
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
    // The dialog takes the new body font and lays out again around it.
    style_dialog_children(window);
    layout_options_dialog(window);
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

  // One flat list in role order; the rows share the list's text inset.
  const HWND tree = GetDlgItem(window, IDC_APPEARANCE_TREE);
  for (std::uint32_t index = 0;
       index < static_cast<std::uint32_t>(
                   layerc::platform::EditorControlRole::count);
       ++index) {
    const auto role = static_cast<layerc::platform::EditorControlRole>(index);
    const std::wstring label(appearance_role_label(role));
    const auto row = SendMessageW(tree, LB_ADDSTRING, 0,
                                  reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(row),
                 static_cast<LPARAM>(role));
  }
  SendMessageW(tree, LB_SETCURSEL, 0, 0);
  show_advanced_appearance(window, false);
  center_over_owner(window);
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
  // The line under the role list warns in the refusal's own words before
  // Apply or OK refuses (DesignSystem.md, Roles).
  const HWND line = GetDlgItem(window, IDC_APPEARANCE_CONTRAST);
  if (const auto failure =
          first_contrast_failure(appearance_editor_.draft().staged()))
    set_status_line(line, layerc::platform::StatusGlyph::warning,
                    contrast_failure_text(*failure));
  else if (colors(layerc::platform::EditorControlRole::window).high_contrast)
    set_status_line(line, layerc::platform::StatusGlyph::accent_2,
                    L"A contrast theme is on, so Windows colors are in use.");
  else
    set_status_line(line, std::nullopt, {});
  // Each color button says the color it paints, which is otherwise only
  // drawn; the contrast line belongs to them and to the role list
  // (DesignSystem.md, Screen readers).
  const std::wstring contrast = window_text(line);
  for (const auto &entry : appearance_fields) {
    const HWND button = GetDlgItem(window, entry.control);
    if (const auto color = appearance_color(window, entry.field))
      set_color_button_name(button, color_hex(native_rgb_value(*color)));
    set_accessible_description(button, contrast);
  }
  set_accessible_description(GetDlgItem(window, IDC_APPEARANCE_TREE), contrast);
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
  // Choose color measures the new color against the ones it pairs with.
  appearance_editor_.set_picker_targets(
      appearance_contrast_targets(appearance, role, field));
  const bool chosen = choose_color(window, selected);
  appearance_editor_.set_picker_targets({});
  if (!chosen)
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
  if (const auto failure = first_contrast_failure(appearance)) {
    // Show the failing role and focus the override to adjust.
    select_role_row(GetDlgItem(window, IDC_APPEARANCE_TREE), failure->role);
    if (!advanced_appearance_shown(window))
      show_advanced_appearance(window, true);
    refresh_appearance_controls(window);
    const auto message = L"Could not save the appearance. " +
                         contrast_failure_text(*failure) +
                         L" Adjust this override or use Reset selected.";
    themed_message_box(window, message.c_str(), L"Appearance",
                       MB_OK | MB_ICONWARNING);
    if (const auto *entry = appearance_field(contrast_pair_field(failure->pair)))
      SetFocus(GetDlgItem(window, entry->control));
    return false;
  }

  if (!appearance_editor_.draft().apply(layerc::platform::write_editor_appearance)) {
    themed_message_box(window, L"Could not save the selected appearance.",
                L"Appearance", MB_OK | MB_ICONWARNING);
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
          id == IDC_EDITOR_CAPTURE_PRIVACY || id == IDC_LAUNCHER_UPDATES)) ||
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
      show_advanced_appearance(window, !advanced_appearance_shown(window));
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
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
