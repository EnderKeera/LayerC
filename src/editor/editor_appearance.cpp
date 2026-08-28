#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"

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

constexpr std::uint32_t appearance_all_fields = [] {
  std::uint32_t result{};
  for (const auto &entry : appearance_fields) result |= entry.field;
  return result;
}();

const AppearanceFieldAction *appearance_field(const std::uint32_t field) {
  const auto found = std::ranges::find(appearance_fields, field,
                                       &AppearanceFieldAction::field);
  return found == appearance_fields.end() ? nullptr : &*found;
}

} // namespace
void EditorWindow::open_options() {
  DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_OPTIONS), dialog_, &dispatch_member_dialog<EditorWindow, &EditorWindow::initialize_options,
                                         &EditorWindow::handle_options_message>,
                  reinterpret_cast<LPARAM>(this));
}

void EditorWindow::initialize_options(const HWND window) {
  set_dark_title_bar(window); style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  set_owner_draw_checkbox(GetDlgItem(window, IDC_CAPTURE_PRIVACY),
                          capture_exclusion_);
  set_owner_draw_checkbox(GetDlgItem(window, IDC_VISUAL_MASTER),
                          layerc::platform::visual_recognition_enabled());
  const HWND preset = GetDlgItem(window, IDC_THEME_PRESET);
  for (const wchar_t *name : {L"Graphite / Copper", L"Graphite / Teal", L"Graphite / Plum"})
    SendMessageW(preset, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
  SendMessageW(preset, LB_SETCURSEL, static_cast<WPARAM>(theme_), 0);
}

void EditorWindow::apply_options(const HWND window) {
  const bool capture =
      owner_draw_checkbox_checked(GetDlgItem(window, IDC_CAPTURE_PRIVACY));
  const bool visual =
      owner_draw_checkbox_checked(GetDlgItem(window, IDC_VISUAL_MASTER));
  const auto selection = static_cast<int>(SendMessageW(GetDlgItem(window, IDC_THEME_PRESET), LB_GETCURSEL, 0, 0));
  const auto theme = static_cast<layerc::platform::EditorTheme>(std::clamp(selection, 0, 2));
  if (!layerc::platform::set_capture_exclusion_enabled(capture) ||
      !layerc::platform::set_visual_recognition_enabled(visual) ||
      !layerc::platform::set_editor_theme(theme)) {
    MessageBoxW(window, L"Could not save the selected options.", L"LayerE", MB_OK | MB_ICONWARNING);
    return;
  }
  capture_exclusion_ = capture; theme_ = theme; accent_ = layerc::platform::editor_accent();
  (void)layerc::platform::apply_capture_exclusion(dialog_, capture_exclusion_);
  if (task_dialog_ != nullptr && IsWindowVisible(task_dialog_))
    (void)layerc::platform::apply_capture_exclusion(task_dialog_, capture_exclusion_);
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
  if (entry == nullptr) return std::nullopt;
  const auto role = appearance_role(window);
  auto overrides = layerc::platform::editor_appearance_overrides(role);
  if ((overrides.fields & field) != 0)
    return color_ref(overrides.*(entry->override_value));
  return colors(role).*(entry->resolved_value);
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
  const auto current = appearance_color(window, field);
  if (!current) return;
  std::uint32_t selected = rgb_value(*current);
  if (!choose_color(window, selected)) return;
  if (auto *value = appearance_value(overrides, field); value != nullptr) {
    *value = selected; overrides.fields |= field;
    if (!layerc::platform::set_editor_appearance_overrides(role, overrides)) {
      MessageBoxW(window, L"Could not save the appearance override.", L"LayerE", MB_OK | MB_ICONWARNING);
      return;
    }
  }
  refresh_theme(); refresh_appearance_controls(window);
}

void EditorWindow::reset_appearance(const HWND window) {
  if (!layerc::platform::reset_editor_appearance_overrides(
          appearance_role(window), appearance_all_fields)) {
    MessageBoxW(window, L"Could not reset this appearance override.", L"LayerE", MB_OK | MB_ICONWARNING);
    return;
  }
  refresh_theme();
  refresh_appearance_controls(window);
}

LRESULT EditorWindow::handle_options_message(const HWND window,
                                             const UINT message,
                                             const WPARAM wparam,
                                             const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND: {
    const int id = LOWORD(wparam);
    if ((id == IDC_CAPTURE_PRIVACY || id == IDC_VISUAL_MASTER) &&
        HIWORD(wparam) == BN_CLICKED) {
      InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
      return TRUE;
    }
    if (id == IDC_OPEN_APPEARANCE) {
      DialogBoxParamW(
          instance_, MAKEINTRESOURCEW(IDD_APPEARANCE), window,
          &dispatch_member_dialog<
              EditorWindow, &EditorWindow::initialize_appearance,
              &EditorWindow::handle_appearance_message>,
          reinterpret_cast<LPARAM>(this));
      refresh_theme();
      return TRUE;
    }
    if (id == IDC_OPTIONS_APPLY) {
      apply_options(window);
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
    if (draw->CtlType == ODT_BUTTON) {
      if (draw->CtlID == IDC_CAPTURE_PRIVACY ||
          draw->CtlID == IDC_VISUAL_MASTER)
        draw_checkbox(*draw);
      else
        draw_button(*draw, layerc::platform::EditorControlRole::button);
      return TRUE;
    }
    break;
  }
  default:
    break;
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
    if (id == IDC_APPEARANCE_TREE && HIWORD(wparam) == LBN_SELCHANGE) {
      refresh_appearance_controls(window);
      return TRUE;
    }
    for (const auto &entry : appearance_fields) {
      if (id == entry.control) {
        choose_appearance_color(window, entry.field);
        return TRUE;
      }
    }
    if (id == IDC_APPEARANCE_RESET) {
      reset_appearance(window);
      return TRUE;
    }
    if (id == IDOK || id == IDCANCEL) {
      EndDialog(window, id);
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
