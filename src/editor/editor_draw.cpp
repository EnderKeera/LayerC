#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include <windows.h>
namespace layerc::editor {
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
  const auto checkbox_colors = colors(layerc::platform::EditorControlRole::checkbox);
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
        ListView_GetCheckState(draw.hwndItem, static_cast<int>(draw.itemID)) != FALSE;
    const int item_height = std::max(1L, row.bottom - row.top);
    const auto metrics = profile_checkbox_metrics(GetDpiForWindow(draw.hwndItem));
    const int desired_box_size =
        std::max(1, metrics.image_size - metrics.inset * 2);
    const int box_size =
        std::min(std::max(1, item_height - metrics.inset * 2),
                 desired_box_size);
    const int box_top = row.top + (item_height - box_size) / 2;
    RECT box{row.left + metrics.inset, box_top,
             row.left + metrics.inset + box_size,
             box_top + box_size};
    const HBRUSH box_fill = CreateSolidBrush(
        checked ? checkbox_colors.selection : checkbox_colors.hover);
    FillRect(draw.hDC, &box, box_fill);
    DeleteObject(box_fill);
    const HBRUSH box_border = CreateSolidBrush(checkbox_colors.border);
    FrameRect(draw.hDC, &box, box_border);
    DeleteObject(box_border);
    if (checked) {
      const HPEN tick =
          CreatePen(PS_SOLID, metrics.pen_width, checkbox_colors.text);
      const HGDIOBJ previous_pen = SelectObject(draw.hDC, tick);
      MoveToEx(draw.hDC, box.left + box_size / 6,
               box.top + box_size / 2, nullptr);
      LineTo(draw.hDC, box.left + box_size * 5 / 12,
             box.bottom - box_size / 4);
      LineTo(draw.hDC, box.right - box_size / 6,
             box.top + box_size / 4);
      SelectObject(draw.hDC, previous_pen);
      DeleteObject(tick);
    }

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
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font_);
  DrawTextW(draw.hDC, label.data(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

LRESULT EditorWindow::color_message(const UINT message, const WPARAM wparam, const LPARAM lparam) const {
  const HWND control = reinterpret_cast<HWND>(lparam); const HDC dc = reinterpret_cast<HDC>(wparam);
  auto role = layerc::platform::EditorControlRole::label;
  if (message == WM_CTLCOLOREDIT) role = layerc::platform::EditorControlRole::edit;
  else if (message == WM_CTLCOLORLISTBOX) role = layerc::platform::EditorControlRole::list;
  else if (control == status_) role = layerc::platform::EditorControlRole::status;
  const auto role_colors = colors(role); SetTextColor(dc, role_colors.text); SetBkColor(dc, role_colors.fill);
  return reinterpret_cast<LRESULT>(brush(role));
}

} // namespace layerc::editor
