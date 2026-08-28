#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include <windows.h>
namespace layerc::editor {
namespace {

RECT paint_checkbox(const HDC dc, const RECT bounds, const bool checked,
                    const RoleColors &colors,
                    const ProfileCheckboxMetrics &metrics) {
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const int desired = std::max(1, metrics.image_size - metrics.inset * 2);
  const int size = std::min(std::max(1, height - metrics.inset * 2), desired);
  const int top = bounds.top + (height - size) / 2;
  RECT box{bounds.left + metrics.inset, top,
           bounds.left + metrics.inset + size, top + size};
  const HBRUSH fill =
      CreateSolidBrush(checked ? colors.selection : colors.hover);
  FillRect(dc, &box, fill);
  DeleteObject(fill);
  const HBRUSH border = CreateSolidBrush(colors.border);
  FrameRect(dc, &box, border);
  DeleteObject(border);
  if (checked) {
    const HPEN tick = CreatePen(PS_SOLID, metrics.pen_width, colors.text);
    const HGDIOBJ previous_pen = SelectObject(dc, tick);
    MoveToEx(dc, box.left + size / 6, box.top + size / 2, nullptr);
    LineTo(dc, box.left + size * 5 / 12, box.bottom - size / 4);
    LineTo(dc, box.right - size / 6, box.top + size / 4);
    SelectObject(dc, previous_pen);
    DeleteObject(tick);
  }
  return box;
}

} // namespace
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
void EditorWindow::draw_profile_item(const DRAWITEMSTRUCT &draw) const {
  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  const auto checkbox_colors =
      colors(layerc::platform::EditorControlRole::checkbox);
  const bool selected = (draw.itemState & ODS_SELECTED) != 0;
  RECT row = draw.rcItem;
  RECT client{};
  GetClientRect(draw.hwndItem, &client);
  row.right = client.right;
  const HBRUSH background =
      CreateSolidBrush(selected ? list_colors.selection : list_colors.fill);
  FillRect(draw.hDC, &row, background);
  DeleteObject(background);

  if (draw.itemID != static_cast<UINT>(-1)) {
    const auto index = static_cast<std::size_t>(draw.itemID);
    const bool enabled = index < entries_.size() && entries_[index].enabled;
    const bool checked =
        ListView_GetCheckState(draw.hwndItem,
                               static_cast<int>(draw.itemID)) != FALSE;
    const auto metrics =
        profile_checkbox_metrics(GetDpiForWindow(draw.hwndItem));
    const RECT box =
        paint_checkbox(draw.hDC, row, checked, checkbox_colors, metrics);

    const std::wstring label =
        index < entries_.size() ? entries_[index].label : std::wstring{};
    RECT text = row;
    text.left = box.right + std::max(1, MulDiv(5, metrics.image_size, 16));
    text.right -= std::max(1, MulDiv(4, metrics.image_size, 16));
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, enabled ? list_colors.text : list_colors.disabled);
    const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
    DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()), &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    SelectObject(draw.hDC, previous_font);
  }

  if ((draw.itemState & ODS_FOCUS) != 0) {
    RECT focus = row;
    InflateRect(&focus, -1, -1);
    const HBRUSH focus_brush = CreateSolidBrush(list_colors.border);
    FrameRect(draw.hDC, &focus, focus_brush);
    DeleteObject(focus_brush);
  }
}
void EditorWindow::draw_checkbox(const DRAWITEMSTRUCT &draw) const {
  const auto role_colors =
      colors(layerc::platform::EditorControlRole::checkbox);
  const bool checked = owner_draw_checkbox_checked(draw.hwndItem);
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const HBRUSH background = CreateSolidBrush(role_colors.fill);
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  const auto metrics =
      profile_checkbox_metrics(GetDpiForWindow(draw.hwndItem));
  const RECT box =
      paint_checkbox(draw.hDC, draw.rcItem, checked, role_colors, metrics);

  std::array<wchar_t, 160> label{};
  GetWindowTextW(draw.hwndItem, label.data(), static_cast<int>(label.size()));
  RECT text = draw.rcItem;
  text.left = box.right + std::max(1, MulDiv(7, metrics.image_size, 16));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC,
               disabled ? role_colors.disabled : role_colors.text);
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
  DrawTextW(draw.hDC, label.data(), -1, &text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(draw.hDC, previous_font);
}
void EditorWindow::draw_appearance_preview(const DRAWITEMSTRUCT &draw, const HWND window) const {
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
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
  SelectObject(draw.hDC, previous_font);
}

LRESULT EditorWindow::color_message(const UINT message, const WPARAM wparam,
                                    const LPARAM lparam) const {
  const HWND control = reinterpret_cast<HWND>(lparam);
  const HDC dc = reinterpret_cast<HDC>(wparam);
  auto role = layerc::platform::EditorControlRole::label;
  if (message == WM_CTLCOLOREDIT)
    role = layerc::platform::EditorControlRole::edit;
  else if (message == WM_CTLCOLORLISTBOX)
    role = layerc::platform::EditorControlRole::list;
  else if (control == status_)
    role = layerc::platform::EditorControlRole::status;
  const auto role_colors = colors(role);
  SetTextColor(dc, role_colors.text);
  SetBkColor(dc, role_colors.fill);
  return reinterpret_cast<LRESULT>(brush(role));
}

LRESULT EditorWindow::erase_dialog_background(const HWND window,
                                              const WPARAM wparam) const {
  RECT client{};
  GetClientRect(window, &client);
  FillRect(reinterpret_cast<HDC>(wparam), &client,
           brush(layerc::platform::EditorControlRole::window));
  return TRUE;
}

std::optional<LRESULT>
EditorWindow::dialog_theme_message(const HWND window, const UINT message,
                                   const WPARAM wparam,
                                   const LPARAM lparam) const {
  if (message == WM_DRAWITEM) {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (item.CtlType == ODT_BUTTON) {
      draw_button(item, layerc::platform::EditorControlRole::button);
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
