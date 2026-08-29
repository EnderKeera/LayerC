#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

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
  for (std::size_t index = 0; index < appearance_presets.size(); ++index) {
    if (appearance_presets[index].theme == theme) {
      SendDlgItemMessageW(window, IDC_THEME_PRESET, CB_SETCURSEL,
                          static_cast<WPARAM>(index), 0);
      return;
    }
  }
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

void show_advanced_appearance(const HWND window, const bool visible) {
  for (const int id : appearance_advanced_controls)
    ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE);
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
      instance_, MAKEINTRESOURCEW(IDD_OPTIONS), dialog_,
      &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_options,
                              &EditorWindow::handle_options_message>,
      reinterpret_cast<LPARAM>(this));
}

void EditorWindow::open_appearance() {
  appearance_.begin(layerc::platform::editor_appearance());
  appearance_open_ = true;
  refresh_theme();
  DialogBoxParamW(
      instance_, MAKEINTRESOURCEW(IDD_APPEARANCE), dialog_,
      &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_appearance,
                              &EditorWindow::handle_appearance_message>,
      reinterpret_cast<LPARAM>(this));
  appearance_open_ = false;
  appearance_.cancel();
  const auto saved = layerc::platform::editor_appearance();
  theme_ = saved.theme;
  accent_ = saved.accent;
  refresh_theme();
}

void EditorWindow::initialize_options(const HWND window) {
  set_dark_title_bar(window);
  style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  CheckDlgButton(window, IDC_VISUAL_MASTER,
                 layerc::platform::visual_recognition_enabled() ? BST_CHECKED
                                                                : BST_UNCHECKED);
  CheckDlgButton(window, IDC_CAPTURE_PRIVACY,
                 capture_exclusion_ ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(window, IDC_OPTIONS_ADVANCED, BST_UNCHECKED);
  ShowWindow(GetDlgItem(window, IDC_CAPTURE_PRIVACY), SW_HIDE);
}

void EditorWindow::apply_options(const HWND window) {
  const bool capture =
      IsDlgButtonChecked(window, IDC_CAPTURE_PRIVACY) == BST_CHECKED;
  const bool visual =
      IsDlgButtonChecked(window, IDC_VISUAL_MASTER) == BST_CHECKED;
  bool saved = layerc::platform::set_capture_exclusion_enabled(capture);
  if (!layerc::platform::set_visual_recognition_enabled(visual))
    saved = false;
  if (!saved) {
    MessageBoxW(window, L"Could not save the selected options.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return;
  }

  capture_exclusion_ = capture;
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  (void)layerc::platform::apply_capture_exclusion(dialog_, capture_exclusion_);
  if (task_dialog_ != nullptr && IsWindow(task_dialog_))
    (void)layerc::platform::apply_capture_exclusion(task_dialog_,
                                                    capture_exclusion_);
  (void)layerc::platform::signal_settings_reload();
  set_status(L"Options saved.");
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
  set_dark_title_bar(window);
  style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);

  const HWND preset = GetDlgItem(window, IDC_THEME_PRESET);
  for (const auto &entry : appearance_presets) {
    const LRESULT row = SendMessageW(
        preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name));
    if (row >= 0)
      SendMessageW(preset, CB_SETITEMDATA, static_cast<WPARAM>(row),
                   static_cast<LPARAM>(entry.theme));
  }
  select_appearance_preset(window, appearance_.staged().theme);

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
  CheckDlgButton(window, IDC_APPEARANCE_ADVANCED, BST_UNCHECKED);
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
  const auto &appearance = appearance_.staged();
  return layerc::platform::native_theme_colors(appearance.theme,
                                               appearance.accent, role,
                                               appearance.overrides[static_cast<std::size_t>(role)])
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
  auto &appearance = appearance_.staged();
  const auto current = appearance_color(window, field);
  if (!current)
    return;
  std::uint32_t selected = rgb_value(*current);
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
  auto &appearance = appearance_.staged();
  appearance.overrides[index].fields &= ~appearance_all_fields;
  refresh_theme();
  refresh_appearance_controls(window);
}

void EditorWindow::reset_all_appearance(const HWND window) {
  auto &appearance = appearance_.staged();
  appearance.theme = layerc::platform::EditorTheme::copper;
  for (auto &overrides : appearance.overrides)
    overrides = {};
  select_appearance_preset(window, appearance.theme);
  refresh_theme();
  refresh_appearance_controls(window);
}

bool EditorWindow::apply_appearance(const HWND window) {
  const auto &appearance = appearance_.staged();
  for (std::size_t index = 0; index < appearance.overrides.size(); ++index) {
    const auto role =
        static_cast<layerc::platform::EditorControlRole>(index);
    const auto role_colors = layerc::platform::native_theme_colors(
        appearance.theme, appearance.accent, role, appearance.overrides[index]);
    if (contrast_ratio(role_colors.text, role_colors.fill) < 4.5 ||
        contrast_ratio(role_colors.disabled, role_colors.fill) < 3.0) {
      MessageBoxW(
          window,
          L"Text must have at least 4.5:1 contrast and disabled text at least "
          L"3:1 contrast against each control fill.",
          L"LayerC Editor", MB_OK | MB_ICONWARNING);
      return false;
    }
  }

  if (!appearance_.apply(layerc::platform::set_editor_appearance)) {
    MessageBoxW(window, L"Could not save the selected appearance.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return false;
  }

  theme_ = appearance.theme;
  accent_ = appearance.accent;
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
    if (id == IDC_OPTIONS_ADVANCED && HIWORD(wparam) == BN_CLICKED) {
      const bool visible =
          IsDlgButtonChecked(window, IDC_OPTIONS_ADVANCED) == BST_CHECKED;
      ShowWindow(GetDlgItem(window, IDC_CAPTURE_PRIVACY),
                 visible ? SW_SHOW : SW_HIDE);
      return TRUE;
    }
    if (id == IDC_OPTIONS_APPLY && HIWORD(wparam) == BN_CLICKED) {
      apply_options(window);
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
          appearance_.staged().theme =
              static_cast<layerc::platform::EditorTheme>(theme);
      }
      refresh_theme();
      refresh_appearance_controls(window);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_ADVANCED && notification == BN_CLICKED) {
      show_advanced_appearance(
          window, IsDlgButtonChecked(window, IDC_APPEARANCE_ADVANCED) ==
                      BST_CHECKED);
      return TRUE;
    }
    if (id == IDC_APPEARANCE_TREE && notification == LBN_SELCHANGE) {
      refresh_appearance_controls(window);
      return TRUE;
    }
    for (const auto &entry : appearance_fields) {
      if (id == entry.control && notification == BN_CLICKED) {
        choose_appearance_color(window, entry.field);
        return TRUE;
      }
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
      for (const auto &entry : appearance_fields) {
        if (static_cast<int>(draw->CtlID) == entry.control)
          fill = appearance_color(window, entry.field);
      }
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
