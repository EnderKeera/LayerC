#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
namespace {

using layerc::platform::EditorControlRole;

HFONT control_font(const HWND control) {
  const auto font =
      reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
  // Controls without an assigned font use the native default, never another
  // window's DPI-specific font.
  return font != nullptr ? font
                         : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

int scaled(const int logical, const UINT dpi) {
  return MulDiv(logical, static_cast<int>(dpi), 96);
}

// Rows: pointer hover lifts the fill; the selection is a quiet accent tint
// marked by a 3 px accent rail (the accent's list role).
void paint_row_background(const HDC dc, const RECT &row, const bool selected,
                          const bool hot, const RoleColors &colors,
                          const UINT dpi) {
  fill_solid_rect(dc, row,
                  selected ? colors.selection
                  : hot    ? colors.hover
                           : colors.fill);
  if (selected) {
    RECT rail = row;
    rail.right = rail.left + std::max(2, scaled(3, dpi));
    fill_solid_rect(dc, rail, colors.accent);
  }
}

void draw_text(const HDC dc, const HFONT font, const std::wstring_view text,
               RECT bounds, const COLORREF color, const UINT format) {
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  const HGDIOBJ previous = SelectObject(dc, font);
  DrawTextW(dc, text.data(), static_cast<int>(text.size()), &bounds, format);
  SelectObject(dc, previous);
}

constexpr UINT single_line = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                             DT_END_ELLIPSIS | DT_NOPREFIX;

// Square 1 px outlined rectangle filled with `fill`, like the buttons.
void outlined_rect(const HDC dc, const RECT &bounds, const COLORREF fill,
                   const COLORREF outline) {
  layerc::platform::fill_solid_rect(dc, bounds, fill);
  layerc::platform::frame_solid_rect(dc, bounds, outline);
}

} // namespace

void EditorWindow::draw_button(const DRAWITEMSTRUCT &draw,
                               const layerc::platform::EditorControlRole role,
                               const std::optional<COLORREF> explicit_fill) const {
  layerc::platform::paint_native_button(draw, colors(role),
                                        control_font(draw.hwndItem), explicit_fill);
}

void EditorWindow::draw_list_box_item(const DRAWITEMSTRUCT &draw) const {
  const auto list_colors = colors(EditorControlRole::list);
  const UINT dpi = native_window_dpi(draw.hwndItem);
  const bool combo_field = (draw.itemState & ODS_COMBOBOXEDIT) != 0;
  const bool selected = !combo_field && (draw.itemState & ODS_SELECTED) != 0;
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool hot = !combo_field && draw.itemID != static_cast<UINT>(-1) &&
                   layerc::platform::native_list_hot_item(draw.hwndItem) ==
                       static_cast<int>(draw.itemID);
  paint_row_background(draw.hDC, draw.rcItem, selected, hot, list_colors, dpi);
  if (draw.itemID != static_cast<UINT>(-1)) {
    const bool combo = draw.CtlType == ODT_COMBOBOX;
    const UINT length_message = combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
    const UINT text_message = combo ? CB_GETLBTEXT : LB_GETTEXT;
    const LRESULT length =
        SendMessageW(draw.hwndItem, length_message, draw.itemID, 0);
    if (length != (combo ? CB_ERR : LB_ERR)) {
      std::wstring label(static_cast<std::size_t>(length) + 1U, L'\0');
      SendMessageW(draw.hwndItem, text_message, draw.itemID,
                   reinterpret_cast<LPARAM>(label.data()));
      label.resize(static_cast<std::size_t>(length));
      RECT text_rect = draw.rcItem;
      text_rect.left += scaled(combo_field ? 4 : 10, dpi);
      text_rect.right -= scaled(6, dpi);
      draw_text(draw.hDC, control_font(draw.hwndItem), label, text_rect,
                disabled ? list_colors.disabled : list_colors.text,
                single_line);
    }
  }
  if (!combo_field && (draw.itemState & ODS_FOCUS) != 0 &&
      (draw.itemState & ODS_NOFOCUSRECT) == 0)
    layerc::platform::paint_focus_ring(draw.hDC, draw.rcItem,
                                       list_colors.accent, dpi);
}

void EditorWindow::draw_profile_item(const DRAWITEMSTRUCT &draw) const {
  // The saved-profiles list paints with the Saved Profiles (sidebar) role.
  const auto list_colors = colors(EditorControlRole::sidebar);
  const UINT dpi = native_window_dpi(draw.hwndItem);
  const bool selected = (draw.itemState & ODS_SELECTED) != 0;
  RECT row = draw.rcItem;
  RECT client{};
  GetClientRect(draw.hwndItem, &client);
  row.right = client.right;
  const bool hot = draw.itemID != static_cast<UINT>(-1) &&
                   layerc::platform::native_list_hot_item(draw.hwndItem) ==
                       static_cast<int>(draw.itemID);
  paint_row_background(draw.hDC, row, selected, hot, list_colors, dpi);
  if (draw.itemID == static_cast<UINT>(-1))
    return;

  const auto index = static_cast<std::size_t>(draw.itemID);
  const ProfileEntry *entry =
      index < shell_.profiles.size() ? &shell_.profiles[index] : nullptr;
  const bool checked = ListView_GetCheckState(
                           draw.hwndItem, static_cast<int>(draw.itemID)) != FALSE;
  const auto metrics = profile_checkbox_metrics(dpi);
  RECT cell = row;
  cell.left += metrics.inset;
  cell.right = row.left + metrics.image_size;
  const RECT box = layerc::platform::paint_native_check_box(
      draw.hDC, cell,
      {.checked = checked,
       .disabled = (draw.itemState & ODS_DISABLED) != 0,
       .hot = hot,
       .pressed = false,
       .radio = false},
      list_colors, dpi);

  // An off profile is a state, not an unavailable control: normal text plus
  // an "Off" tag, never the disabled color.
  RECT text = row;
  text.left = box.right + scaled(8, dpi);
  text.right -= scaled(8, dpi);
  if (entry != nullptr && !entry->enabled) {
    RECT tag = text;
    tag.left = std::max(text.left, text.right - scaled(28, dpi));
    draw_text(draw.hDC, shell_.font, L"Off", tag, list_colors.secondary,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    text.right = tag.left - scaled(4, dpi);
  }
  if (entry != nullptr) {
    RECT measured = text;
    const HGDIOBJ previous = SelectObject(draw.hDC, shell_.font);
    DrawTextW(draw.hDC, entry->name.c_str(),
              static_cast<int>(entry->name.size()), &measured,
              single_line | DT_CALCRECT);
    SelectObject(draw.hDC, previous);
    draw_text(draw.hDC, shell_.font, entry->name, text, list_colors.text,
              single_line);
    if (!entry->executable.empty() && measured.right + scaled(24, dpi) < text.right) {
      RECT executable = text;
      executable.left = measured.right + scaled(8, dpi);
      draw_text(draw.hDC, shell_.font, entry->executable, executable,
                list_colors.secondary, single_line);
    }
  }
  if ((draw.itemState & ODS_FOCUS) != 0 &&
      (draw.itemState & ODS_NOFOCUSRECT) == 0)
    layerc::platform::paint_focus_ring(draw.hDC, row, list_colors.accent, dpi);
}

// A miniature editor in the staged appearance: every role in place, with the
// role being edited outlined in the accent.
void EditorWindow::draw_appearance_preview(const DRAWITEMSTRUCT &draw,
                                           const HWND window) const {
  const UINT dpi = native_window_dpi(draw.hwndItem);
  const auto active = appearance_role(window);
  const HDC dc = draw.hDC;
  const RECT area = draw.rcItem;
  const auto role = [this](const EditorControlRole value) {
    return colors(value);
  };
  const auto mark = [&](const EditorControlRole value, RECT bounds) {
    if (value != active)
      return;
    InflateRect(&bounds, 2, 2);
    layerc::platform::paint_focus_ring(dc, bounds, role(value).accent, dpi);
  };
  const int s4 = scaled(4, dpi);
  const int s8 = scaled(8, dpi);
  const int row_height = scaled(26, dpi);

  const auto window_colors = role(EditorControlRole::window);
  fill_solid_rect(dc, area, window_colors.fill);
  mark(EditorControlRole::window, area);

  RECT toolbar{area.left, area.top, area.right, area.top + scaled(24, dpi)};
  const auto toolbar_colors = role(EditorControlRole::toolbar);
  fill_solid_rect(dc, toolbar, toolbar_colors.fill);
  RECT menu_text = toolbar;
  menu_text.left += s8;
  draw_text(dc, shell_.font, L"File   Edit   Tools   Help", menu_text,
            toolbar_colors.text, single_line);
  mark(EditorControlRole::toolbar, toolbar);

  RECT status{area.left, area.bottom - scaled(22, dpi), area.right, area.bottom};
  const auto status_colors = role(EditorControlRole::status);
  fill_solid_rect(dc, status, status_colors.fill);
  RECT status_line = status;
  status_line.bottom = status_line.top + 1;
  fill_solid_rect(dc, status_line, status_colors.divider);
  RECT status_text = status;
  status_text.left += s8;
  draw_text(dc, shell_.font, L"Profile saved.", status_text,
            status_colors.text, single_line);
  mark(EditorControlRole::status, status);

  const int body_top = toolbar.bottom + s8;
  const int sidebar_width = std::min(
      scaled(170, dpi), static_cast<int>(area.right - area.left) / 3);
  RECT sidebar{area.left + s8, body_top, area.left + s8 + sidebar_width,
               status.top - s8};
  const auto sidebar_colors = role(EditorControlRole::sidebar);
  fill_solid_rect(dc, sidebar, sidebar_colors.fill);
  frame_solid_rect(dc, sidebar, sidebar_colors.border);
  const std::array<std::wstring_view, 3> profiles{L"Arena", L"Raid",
                                                   L"Practice"};
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    RECT item{sidebar.left + 1,
              sidebar.top + 1 + static_cast<int>(index) * row_height,
              sidebar.right - 1,
              sidebar.top + 1 + static_cast<int>(index + 1) * row_height};
    if (item.bottom > sidebar.bottom)
      break;
    paint_row_background(dc, item, index == 0, index == 1, sidebar_colors, dpi);
    RECT text = item;
    text.left += scaled(12, dpi);
    draw_text(dc, shell_.font, profiles[index], text, sidebar_colors.text,
              single_line);
  }
  mark(EditorControlRole::sidebar, sidebar);

  const int left = sidebar.right + scaled(16, dpi);
  const int right = area.right - s8;
  int top = body_top;
  const auto label_colors = role(EditorControlRole::label);
  RECT label{left, top, left + scaled(90, dpi), top + row_height};
  draw_text(dc, shell_.font, L"Name", label, label_colors.text, single_line);
  mark(EditorControlRole::label, label);

  const auto edit_colors = role(EditorControlRole::edit);
  RECT edit{label.right + s4, top,
            std::min<LONG>(right, label.right + scaled(220, dpi)),
            top + row_height};
  fill_solid_rect(dc, edit, edit_colors.fill);
  frame_solid_rect(dc, edit, edit_colors.border);
  RECT accent_edge{edit.left, edit.bottom - 2, edit.right, edit.bottom};
  fill_solid_rect(dc, accent_edge, edit_colors.accent);
  RECT edit_text = edit;
  edit_text.left += s8;
  draw_text(dc, shell_.font, L"Dash", edit_text, edit_colors.text, single_line);
  mark(EditorControlRole::edit, edit);

  top += row_height + s8;
  const auto button_colors = role(EditorControlRole::button);
  const int button_width = scaled(84, dpi);
  const std::array<std::pair<std::wstring_view, int>, 3> buttons{
      {{L"Button", 0}, {L"Hover", 1}, {L"Save", 2}}};
  int button_left = left;
  RECT first_button{};
  for (const auto &[text, kind] : buttons) {
    RECT button{button_left, top, button_left + button_width,
                top + scaled(30, dpi)};
    if (button.right > right)
      break;
    const COLORREF fill = kind == 2   ? button_colors.accent
                          : kind == 1 ? button_colors.hover
                                      : button_colors.fill;
    outlined_rect(dc, button, fill, kind == 2 ? fill : button_colors.border);
    draw_text(dc, shell_.font, text, button,
              kind == 2 ? button_colors.on_accent : button_colors.text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (kind == 0)
      first_button = button;
    button_left = button.right + s8;
  }
  mark(EditorControlRole::button, first_button);

  top += scaled(30, dpi) + s8;
  const auto check_colors = role(EditorControlRole::checkbox);
  RECT check{left, top, left + scaled(150, dpi), top + row_height};
  const RECT box = layerc::platform::paint_native_check_box(
      dc, check, {.checked = true}, check_colors, dpi);
  RECT check_text = check;
  check_text.left = box.right + s8;
  draw_text(dc, shell_.font, L"Show text", check_text, check_colors.text,
            single_line);
  RECT disabled_text{check.right + s8, top, right, top + row_height};
  draw_text(dc, shell_.font, L"Unavailable", disabled_text,
            check_colors.disabled, single_line);
  mark(EditorControlRole::checkbox, check);

  top += row_height + s8;
  const auto list_colors = role(EditorControlRole::list);
  const auto preview_colors = role(EditorControlRole::preview);
  const int half = std::max(1, (right - left - s8) / 2);
  RECT list{left, top, left + half,
            std::min<LONG>(status.top - s8, top + row_height * 2 + 2)};
  if (list.bottom > list.top + 4) {
    fill_solid_rect(dc, list, list_colors.fill);
    frame_solid_rect(dc, list, list_colors.border);
    RECT selected_row{list.left + 1, list.top + 1, list.right - 1,
                      std::min(list.bottom - 1, list.top + 1 + row_height)};
    paint_row_background(dc, selected_row, true, false, list_colors, dpi);
    RECT row_text = selected_row;
    row_text.left += scaled(12, dpi);
    draw_text(dc, shell_.font, L"Selected row", row_text, list_colors.text,
              single_line);
    mark(EditorControlRole::list, list);

    RECT preview{list.right + s8, list.top, right, list.bottom};
    fill_solid_rect(dc, preview, preview_colors.fill);
    frame_solid_rect(dc, preview, preview_colors.border);
    RECT bar{preview.left + s8, preview.top + s8,
             preview.left + s8 + (preview.right - preview.left - s8 * 2) * 2 / 3,
             preview.top + s8 + scaled(10, dpi)};
    if (bar.bottom < preview.bottom && bar.right > bar.left)
      fill_solid_rect(dc, bar, preview_colors.accent);
    mark(EditorControlRole::preview, preview);
  }
}

LRESULT EditorWindow::color_message(const UINT message, const WPARAM wparam,
                                    const LPARAM lparam) const {
  const HWND control = reinterpret_cast<HWND>(lparam);
  const HDC dc = reinterpret_cast<HDC>(wparam);
  auto role = EditorControlRole::label;
  bool heading = false;
  bool unavailable = false;
  if (message == WM_CTLCOLOREDIT) {
    role = EditorControlRole::edit;
  } else if (message == WM_CTLCOLORLISTBOX) {
    role = EditorControlRole::list;
  } else if (message == WM_CTLCOLORBTN) {
    const auto type =
        GetWindowLongPtrW(control, GWL_STYLE) & static_cast<LONG_PTR>(BS_TYPEMASK);
    role = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON ||
                   type == BS_OWNERDRAW
               ? EditorControlRole::button
               : EditorControlRole::checkbox;
  } else if (control == shell_.status) {
    role = EditorControlRole::status;
  } else if (layerc::platform::window_class(control) == L"Edit") {
    // Read-only and disabled edits report WM_CTLCOLORSTATIC; keep them on
    // the input surface so they still read as fields.
    role = EditorControlRole::edit;
    unavailable = IsWindowEnabled(control) == FALSE;
  } else {
    heading = shell_.heading_font != nullptr &&
              reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)) ==
                  shell_.heading_font;
    unavailable = IsWindowEnabled(control) == FALSE;
  }
  const auto role_colors = colors(role);
  // Section headings use primary text; labels and hints the secondary tone.
  SetTextColor(dc, unavailable ? role_colors.disabled
                   : heading   ? colors(EditorControlRole::window).text
                               : role_colors.text);
  SetBkColor(dc, role_colors.fill);
  return reinterpret_cast<LRESULT>(brush(role));
}

LRESULT EditorWindow::erase_dialog_background(const HWND window,
                                              const WPARAM wparam) const {
  RECT client{};
  GetClientRect(window, &client);
  FillRect(reinterpret_cast<HDC>(wparam), &client,
           brush(EditorControlRole::window));
  return TRUE;
}

std::optional<LRESULT>
EditorWindow::dialog_theme_message(const HWND window, const UINT message,
                                   const WPARAM wparam,
                                   const LPARAM lparam) const {
  if (message == WM_DRAWITEM) {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (item.CtlType == ODT_BUTTON) {
      draw_button(item, EditorControlRole::button);
      return TRUE;
    }
    if (item.CtlType == ODT_LISTBOX || item.CtlType == ODT_COMBOBOX) {
      draw_list_box_item(item);
      return TRUE;
    }
  } else if (message == WM_NOTIFY) {
    // Checkboxes and radio buttons keep their native type (UIA Toggle) and
    // paint through custom draw. Dialog procedures return notify results
    // through DWLP_MSGRESULT.
    const auto &header = *reinterpret_cast<const NMHDR *>(lparam);
    if (const auto result = layerc::platform::native_button_custom_draw(
            header, colors(EditorControlRole::checkbox),
            control_font(header.hwndFrom))) {
      SetWindowLongPtrW(window, DWLP_MSGRESULT, *result);
      return TRUE;
    }
  } else if (message >= WM_CTLCOLORMSGBOX &&
             message <= WM_CTLCOLORSTATIC) {
    return color_message(message, wparam, lparam);
  } else if (message == WM_ERASEBKGND) {
    return erase_dialog_background(window, wparam);
  }
  return std::nullopt;
}

} // namespace layerc::editor
