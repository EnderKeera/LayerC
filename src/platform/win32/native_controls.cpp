#include "layerc/platform/native_theme.hpp"

#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <mutex>

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace layerc::platform {
namespace {

constexpr UINT_PTR hover_subclass_id = 0x4C43'0001;
constexpr UINT_PTR list_hover_subclass_id = 0x4C43'0002;
constexpr UINT_PTR frame_subclass_id = 0x4C43'0003;
constexpr UINT_PTR menu_bar_subclass_id = 0x4C43'0004;
constexpr UINT_PTR list_header_subclass_id = 0x4C43'0005;

// Undocumented but stable since Windows Vista; used by win32-darkmode and
// Notepad++ to paint the menu bar.
constexpr UINT wm_uah_draw_menu = 0x0091;
constexpr UINT wm_uah_draw_menu_item = 0x0092;

struct UahMenu {
  HMENU menu;
  HDC dc;
  DWORD flags;
};

union UahMenuItemMetrics {
  struct {
    DWORD cx;
    DWORD cy;
  } bar[2];
  struct {
    DWORD cx;
    DWORD cy;
  } popup[4];
};

struct UahMenuPopupMetrics {
  DWORD widths[4];
  DWORD update_max_widths : 2;
};

struct UahMenuItem {
  int position;
  UahMenuItemMetrics metrics;
  UahMenuPopupMetrics popup_metrics;
};

struct UahDrawMenuItem {
  DRAWITEMSTRUCT draw;
  UahMenu menu;
  UahMenuItem item;
};

std::mutex palette_mutex;
NativeChromePalette chrome_palette = [] {
  const EditorAppearanceOverrides none{};
  return NativeChromePalette{
      .frame = native_theme_colors(EditorTheme::copper, 0,
                                   EditorControlRole::edit, none),
      .menu = native_theme_colors(EditorTheme::copper, 0,
                                  EditorControlRole::toolbar, none),
      .button = native_theme_colors(EditorTheme::copper, 0,
                                    EditorControlRole::button, none),
  };
}();

LRESULT CALLBACK hover_subclass(const HWND window, const UINT message,
                                const WPARAM wparam, const LPARAM lparam,
                                const UINT_PTR id, const DWORD_PTR hot) {
  switch (message) {
  case WM_MOUSEMOVE:
    if (hot == 0) {
      SetWindowSubclass(window, &hover_subclass, id, 1);
      TRACKMOUSEEVENT track{.cbSize = sizeof(TRACKMOUSEEVENT),
                            .dwFlags = TME_LEAVE,
                            .hwndTrack = window,
                            .dwHoverTime = 0};
      TrackMouseEvent(&track);
      InvalidateRect(window, nullptr, FALSE);
    }
    break;
  case WM_MOUSELEAVE:
    SetWindowSubclass(window, &hover_subclass, id, 0);
    InvalidateRect(window, nullptr, FALSE);
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, &hover_subclass, id);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

int list_item_at(const HWND list, const POINT point, const bool list_view) {
  if (list_view) {
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int row = ListView_HitTest(list, &hit);
    return row >= 0 && (hit.flags & LVHT_ONITEM) != 0 ? row : -1;
  }
  const LRESULT result = SendMessageW(
      list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
  return HIWORD(result) == 0 ? static_cast<int>(LOWORD(result)) : -1;
}

void invalidate_list_item(const HWND list, const int item,
                          const bool list_view) {
  if (item < 0)
    return;
  RECT rect{};
  if (list_view) {
    if (ListView_GetItemRect(list, item, &rect, LVIR_BOUNDS) == FALSE)
      return;
    RECT client{};
    GetClientRect(list, &client);
    rect.left = client.left;
    rect.right = client.right;
  } else if (SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(item),
                          reinterpret_cast<LPARAM>(&rect)) == LB_ERR) {
    return;
  }
  InvalidateRect(list, &rect, FALSE);
}

// Reference data holds hovered row + 1 (0: none).
LRESULT CALLBACK list_hover_subclass(const HWND window, const UINT message,
                                     const WPARAM wparam, const LPARAM lparam,
                                     const UINT_PTR id,
                                     const DWORD_PTR hot_plus_one) {
  const bool list_view = window_class(window) == WC_LISTVIEWW;
  const int previous = static_cast<int>(hot_plus_one) - 1;
  const auto set_hot = [&](const int item) {
    if (item == previous)
      return;
    SetWindowSubclass(window, &list_hover_subclass, id,
                      static_cast<DWORD_PTR>(item + 1));
    invalidate_list_item(window, previous, list_view);
    invalidate_list_item(window, item, list_view);
  };
  switch (message) {
  case WM_MOUSEMOVE: {
    if (previous < 0) {
      TRACKMOUSEEVENT track{.cbSize = sizeof(TRACKMOUSEEVENT),
                            .dwFlags = TME_LEAVE,
                            .hwndTrack = window,
                            .dwHoverTime = 0};
      TrackMouseEvent(&track);
    }
    set_hot(list_item_at(window, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)},
                         list_view));
    break;
  }
  case WM_MOUSELEAVE:
    set_hot(-1);
    break;
  case WM_MOUSEWHEEL:
  case WM_VSCROLL:
    set_hot(-1);
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, &list_hover_subclass, id);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void paint_flat_frame(const HWND window) {
  RECT bounds{};
  GetWindowRect(window, &bounds);
  OffsetRect(&bounds, -bounds.left, -bounds.top);
  const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  int thickness = 0;
  if ((ex_style & WS_EX_CLIENTEDGE) != 0)
    thickness += GetSystemMetrics(SM_CXEDGE);
  if ((style & WS_BORDER) != 0)
    thickness += GetSystemMetrics(SM_CXBORDER);
  if (thickness == 0)
    return;
  const auto palette = native_chrome_palette().frame;
  const bool focused = GetFocus() == window;
  const bool enabled = IsWindowEnabled(window) != FALSE;
  const HDC dc = GetWindowDC(window);
  if (dc == nullptr)
    return;
  // Outer 1 px role border, then the remainder of the edge in input fill.
  frame_solid_rect(dc, bounds, enabled ? palette.border : palette.divider);
  RECT inner = bounds;
  for (int ring = 1; ring < thickness; ++ring) {
    InflateRect(&inner, -1, -1);
    frame_solid_rect(dc, inner, palette.fill);
  }
  if (focused && enabled) {
    const int accent_height = std::max(2, MulDiv(2, static_cast<int>(native_window_dpi(window)), 96));
    RECT accent{bounds.left, bounds.bottom - accent_height, bounds.right,
                bounds.bottom};
    fill_solid_rect(dc, accent, palette.accent);
  }
  ReleaseDC(window, dc);
}

// The DarkMode_ItemsView header theme darkens the header strip but leaves its
// text black; the header asks its list view (the parent) how to paint, so the
// list answers with its own text color.
LRESULT CALLBACK list_header_subclass(const HWND window, const UINT message,
                                      const WPARAM wparam, const LPARAM lparam,
                                      const UINT_PTR id, DWORD_PTR) {
  if (message == WM_NOTIFY) {
    const auto &header = *reinterpret_cast<const NMHDR *>(lparam);
    if (header.code == NM_CUSTOMDRAW &&
        header.hwndFrom == ListView_GetHeader(window)) {
      const auto &draw = reinterpret_cast<const NMCUSTOMDRAW &>(header);
      if (draw.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
      if (draw.dwDrawStage == CDDS_ITEMPREPAINT) {
        SetTextColor(draw.hdc, ListView_GetTextColor(window));
        return CDRF_DODEFAULT;
      }
    }
  } else if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, &list_header_subclass, id);
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK frame_subclass(const HWND window, const UINT message,
                                const WPARAM wparam, const LPARAM lparam,
                                const UINT_PTR id, DWORD_PTR) {
  switch (message) {
  case WM_NCPAINT: {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    paint_flat_frame(window);
    return result;
  }
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_ENABLE: {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    RedrawWindow(window, nullptr, nullptr,
                 RDW_FRAME | RDW_INVALIDATE | RDW_NOCHILDREN);
    return result;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, &frame_subclass, id);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

RECT menu_bar_rect(const HWND window) {
  MENUBARINFO info{};
  info.cbSize = sizeof(info);
  if (GetMenuBarInfo(window, OBJID_MENU, 0, &info) == FALSE)
    return {};
  RECT window_rect{};
  GetWindowRect(window, &window_rect);
  RECT bar = info.rcBar;
  OffsetRect(&bar, -window_rect.left, -window_rect.top);
  return bar;
}

void paint_menu_bar_line(const HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&client), 2);
  RECT window_rect{};
  GetWindowRect(window, &window_rect);
  OffsetRect(&client, -window_rect.left, -window_rect.top);
  RECT line = client;
  line.bottom = line.top;
  line.top -= 1;
  const HDC dc = GetWindowDC(window);
  if (dc == nullptr)
    return;
  fill_solid_rect(dc, line, native_chrome_palette().menu.divider);
  ReleaseDC(window, dc);
}

void paint_menu_item(const UahDrawMenuItem &item) {
  const auto palette = native_chrome_palette().menu;
  std::array<wchar_t, 128> text{};
  MENUITEMINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = MIIM_STRING;
  info.dwTypeData = text.data();
  info.cch = static_cast<UINT>(text.size() - 1);
  GetMenuItemInfoW(item.menu.menu, static_cast<UINT>(item.item.position), TRUE,
                   &info);
  const UINT state = item.draw.itemState;
  const bool disabled =
      (state & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) != 0;
  const bool hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
  fill_solid_rect(item.menu.dc, item.draw.rcItem,
                  hot && !disabled ? palette.hover : palette.fill);
  RECT text_rect = item.draw.rcItem;
  SetBkMode(item.menu.dc, TRANSPARENT);
  SetTextColor(item.menu.dc, disabled ? palette.disabled : palette.text);
  UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
  if ((state & ODS_NOACCEL) != 0)
    format |= DT_HIDEPREFIX;
  DrawTextW(item.menu.dc, text.data(), -1, &text_rect, format);
}

LRESULT CALLBACK menu_bar_subclass(const HWND window, const UINT message,
                                   const WPARAM wparam, const LPARAM lparam,
                                   const UINT_PTR id, DWORD_PTR) {
  switch (message) {
  case wm_uah_draw_menu: {
    const auto *menu = reinterpret_cast<const UahMenu *>(lparam);
    RECT bar = menu_bar_rect(window);
    fill_solid_rect(menu->dc, bar, native_chrome_palette().menu.fill);
    return TRUE;
  }
  case wm_uah_draw_menu_item:
    paint_menu_item(*reinterpret_cast<const UahDrawMenuItem *>(lparam));
    return TRUE;
  case WM_NCPAINT:
  case WM_NCACTIVATE: {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    paint_menu_bar_line(window);
    return result;
  }
  case WM_NCDESTROY:
    RemoveWindowSubclass(window, &menu_bar_subclass, id);
    break;
  default:
    break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void style_button_child(const HWND child) {
  const auto style = GetWindowLongPtrW(child, GWL_STYLE);
  const auto button_type = style & static_cast<LONG_PTR>(BS_TYPEMASK);
  if (button_type == BS_OWNERDRAW) {
    track_native_button_hover(child);
    return;
  }
  if (button_type != BS_PUSHBUTTON && button_type != BS_DEFPUSHBUTTON) {
    SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    return;
  }
  SetWindowLongPtrW(child, GWL_STYLE,
                    (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) |
                        BS_OWNERDRAW);
  SetWindowPos(child, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
  track_native_button_hover(child);
}

LPARAM dialog_item_height(const HWND dialog, const HFONT font) {
  // One text line plus 4 px padding above and below, in the dialog's DPI.
  int text_height = 0;
  if (const HDC dc = GetDC(dialog); dc != nullptr) {
    const HGDIOBJ previous = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    if (GetTextMetricsW(dc, &metrics) != FALSE)
      text_height = metrics.tmHeight;
    SelectObject(dc, previous);
    ReleaseDC(dialog, dc);
  }
  const int padding = MulDiv(8, static_cast<int>(native_window_dpi(dialog)), 96);
  return static_cast<LPARAM>(
      std::max(text_height + padding,
               MulDiv(20, static_cast<int>(native_window_dpi(dialog)), 96)));
}

void style_combo_child(const HWND dialog, const HWND child, const HFONT font,
                       const bool owner_draw_list_boxes) {
  if (owner_draw_list_boxes) {
    const auto style = GetWindowLongPtrW(child, GWL_STYLE);
    SetWindowLongPtrW(child, GWL_STYLE,
                      style | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
    const auto item_height = dialog_item_height(dialog, font);
    SendMessageW(child, CB_SETITEMHEIGHT, 0, item_height);
    SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                 item_height);
  }
  SetWindowTheme(child, L"DarkMode_CFD", nullptr);
  track_native_list_hover(child);
}

void style_listbox_child(const HWND dialog, const HWND child, const HFONT font,
                         const bool owner_draw_list_boxes) {
  if (owner_draw_list_boxes) {
    const auto style = GetWindowLongPtrW(child, GWL_STYLE);
    SetWindowLongPtrW(child, GWL_STYLE,
                      style | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
    SendMessageW(child, LB_SETITEMHEIGHT, 0,
                 dialog_item_height(dialog, font) +
                     MulDiv(4, static_cast<int>(native_window_dpi(dialog)), 96));
  }
  SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
  track_native_list_hover(child);
  install_flat_frame(child);
}

void style_listview_child(const HWND child) {
  SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
  if (const HWND header = ListView_GetHeader(child); header != nullptr)
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
  install_list_header_text(child);
  if (const HWND tooltip = ListView_GetToolTips(child); tooltip != nullptr)
    SetWindowTheme(tooltip, L"DarkMode_Explorer", nullptr);
  install_flat_frame(child);
}

} // namespace

void set_native_chrome_palette(const NativeChromePalette &palette) {
  const std::scoped_lock lock(palette_mutex);
  chrome_palette = palette;
}

NativeChromePalette native_chrome_palette() {
  const std::scoped_lock lock(palette_mutex);
  return chrome_palette;
}

void track_native_button_hover(const HWND button) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(button, &hover_subclass, hover_subclass_id,
                        &existing) == FALSE)
    SetWindowSubclass(button, &hover_subclass, hover_subclass_id, 0);
}

bool native_button_hot(const HWND button) {
  DWORD_PTR hot{};
  return GetWindowSubclass(button, &hover_subclass, hover_subclass_id, &hot) !=
             FALSE &&
         hot != 0;
}

void track_native_list_hover(const HWND list) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(list, &list_hover_subclass, list_hover_subclass_id,
                        &existing) == FALSE)
    SetWindowSubclass(list, &list_hover_subclass, list_hover_subclass_id, 0);
}

int native_list_hot_item(const HWND list) {
  DWORD_PTR hot_plus_one{};
  if (GetWindowSubclass(list, &list_hover_subclass, list_hover_subclass_id,
                        &hot_plus_one) == FALSE)
    return -1;
  return static_cast<int>(hot_plus_one) - 1;
}

void install_flat_frame(const HWND control) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(control, &frame_subclass, frame_subclass_id,
                        &existing) != FALSE)
    return;
  SetWindowSubclass(control, &frame_subclass, frame_subclass_id, 0);
  RedrawWindow(control, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
}

void install_list_header_text(const HWND list) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(list, &list_header_subclass, list_header_subclass_id,
                        &existing) != FALSE)
    return;
  SetWindowSubclass(list, &list_header_subclass, list_header_subclass_id, 0);
  if (const HWND header = ListView_GetHeader(list); header != nullptr)
    InvalidateRect(header, nullptr, TRUE);
}

void install_dark_menu_bar(const HWND window) {
  DWORD_PTR existing{};
  if (GetWindowSubclass(window, &menu_bar_subclass, menu_bar_subclass_id,
                        &existing) != FALSE)
    return;
  SetWindowSubclass(window, &menu_bar_subclass, menu_bar_subclass_id, 0);
  DrawMenuBar(window);
}

void style_native_dialog_children(const HWND dialog, const HFONT font,
                                  const bool owner_draw_list_boxes) {
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    const auto class_name = window_class(child);
    if (class_name == L"Button") {
      style_button_child(child);
    } else if (class_name == L"ComboBox") {
      style_combo_child(dialog, child, font, owner_draw_list_boxes);
    } else if (class_name == L"ListBox") {
      style_listbox_child(dialog, child, font, owner_draw_list_boxes);
    } else if (class_name == WC_LISTVIEWW) {
      style_listview_child(child);
    } else if (class_name == L"Edit") {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
      install_flat_frame(child);
    } else if (class_name == L"ScrollBar" || class_name == TOOLTIPS_CLASSW) {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
  }
}

int native_check_box_size(const UINT dpi) {
  return MulDiv(18, static_cast<int>(dpi == 0 ? 96U : dpi), 96);
}

RECT paint_native_check_box(const HDC dc, const RECT &bounds,
                            const NativeCheckState &state,
                            const NativeThemeColors &colors, const UINT dpi) {
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const int size = std::min(native_check_box_size(dpi), height);
  const int top = bounds.top + (height - size) / 2;
  const RECT box{bounds.left, top, bounds.left + size, top + size};
  const bool filled = state.checked && !state.radio;
  COLORREF fill = colors.input;
  COLORREF frame = colors.border;
  if (state.disabled) {
    fill = filled ? colors.divider : colors.fill;
    frame = colors.divider;
  } else if (filled) {
    fill = state.pressed ? blend_color(colors.accent, colors.fill, 60)
           : state.hot   ? blend_color(colors.accent, colors.text, 40)
                         : colors.accent;
    frame = fill;
  } else if (state.pressed) {
    fill = colors.pressed;
  } else if (state.hot) {
    fill = colors.hover;
    frame = colors.text;
  }
  const int scale = static_cast<int>(dpi == 0 ? 96U : dpi);
  const int radius = MulDiv(state.radio ? size : 6, scale, state.radio ? scale : 96);
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, frame);
  const HGDIOBJ previous_brush = SelectObject(dc, brush);
  const HGDIOBJ previous_pen = SelectObject(dc, pen);
  RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
  SelectObject(dc, previous_pen);
  SelectObject(dc, previous_brush);
  DeleteObject(pen);
  DeleteObject(brush);
  if (state.checked && state.radio) {
    RECT dot = box;
    InflateRect(&dot, -size / 4, -size / 4);
    const COLORREF dot_color = state.disabled ? colors.disabled : colors.accent;
    const HBRUSH dot_brush = CreateSolidBrush(dot_color);
    const HPEN dot_pen = CreatePen(PS_SOLID, 1, dot_color);
    const HGDIOBJ old_brush = SelectObject(dc, dot_brush);
    const HGDIOBJ old_pen = SelectObject(dc, dot_pen);
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(dot_pen);
    DeleteObject(dot_brush);
  } else if (filled) {
    const int pen_width = std::max(2, MulDiv(2, scale, 96));
    const HPEN tick = CreatePen(PS_SOLID | PS_ENDCAP_ROUND | PS_GEOMETRIC,
                                pen_width, state.disabled ? colors.disabled
                                                          : colors.on_accent);
    const HGDIOBJ old_pen = SelectObject(dc, tick);
    MoveToEx(dc, box.left + size * 4 / 16, box.top + size * 9 / 16, nullptr);
    LineTo(dc, box.left + size * 7 / 16, box.top + size * 12 / 16);
    LineTo(dc, box.left + size * 12 / 16, box.top + size * 5 / 16);
    SelectObject(dc, old_pen);
    DeleteObject(tick);
  }
  return box;
}

std::optional<LRESULT>
native_button_custom_draw(const NMHDR &header, const NativeThemeColors &colors,
                          const HFONT font) {
  if (header.code != NM_CUSTOMDRAW || window_class(header.hwndFrom) != L"Button")
    return std::nullopt;
  const auto type = GetWindowLongPtrW(header.hwndFrom, GWL_STYLE) &
                    static_cast<LONG_PTR>(BS_TYPEMASK);
  const bool checkbox = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
                        type == BS_3STATE || type == BS_AUTO3STATE;
  const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
  if (!checkbox && !radio)
    return std::nullopt;
  const auto &draw = reinterpret_cast<const NMCUSTOMDRAW &>(header);
  if (draw.dwDrawStage != CDDS_PREPAINT)
    return CDRF_DODEFAULT;
  const HWND control = header.hwndFrom;
  const UINT dpi = native_window_dpi(control);
  RECT client{};
  GetClientRect(control, &client);
  fill_solid_rect(draw.hdc, client, colors.fill);
  const LRESULT check = SendMessageW(control, BM_GETCHECK, 0, 0);
  const NativeCheckState state{
      .checked = check != BST_UNCHECKED,
      .disabled = IsWindowEnabled(control) == FALSE,
      .hot = (draw.uItemState & CDIS_HOT) != 0,
      .pressed = (draw.uItemState & CDIS_SELECTED) != 0,
      .radio = radio,
  };
  const RECT box = paint_native_check_box(draw.hdc, client, state, colors, dpi);
  std::array<wchar_t, 256> label{};
  GetWindowTextW(control, label.data(), static_cast<int>(label.size()));
  RECT text = client;
  text.left = box.right + MulDiv(8, static_cast<int>(dpi), 96);
  SetBkMode(draw.hdc, TRANSPARENT);
  SetTextColor(draw.hdc, state.disabled ? colors.disabled : colors.text);
  const HGDIOBJ previous_font = SelectObject(draw.hdc, font);
  UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  const LRESULT ui_state = SendMessageW(control, WM_QUERYUISTATE, 0, 0);
  if ((ui_state & UISF_HIDEACCEL) != 0)
    format |= DT_HIDEPREFIX;
  RECT measured = text;
  DrawTextW(draw.hdc, label.data(), -1, &measured, format | DT_CALCRECT);
  DrawTextW(draw.hdc, label.data(), -1, &text, format);
  SelectObject(draw.hdc, previous_font);
  if ((draw.uItemState & CDIS_FOCUS) != 0 && (ui_state & UISF_HIDEFOCUS) == 0) {
    RECT focus{box.left, client.top,
               std::min(client.right,
                        measured.right + MulDiv(4, static_cast<int>(dpi), 96)),
               client.bottom};
    paint_focus_ring(draw.hdc, focus, colors.accent, dpi);
  }
  return CDRF_SKIPDEFAULT;
}

void paint_native_button(const DRAWITEMSTRUCT &draw,
                         const NativeThemeColors &colors, const HFONT font,
                         const std::optional<COLORREF> explicit_fill) {
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
  const bool hot = native_button_hot(draw.hwndItem) ||
                   (draw.itemState & ODS_HOTLIGHT) != 0;
  const UINT dpi = native_window_dpi(draw.hwndItem);
  // The one primary action per surface (the dialog's default id) carries
  // the accent; every other button is neutral.
  const HWND parent = GetParent(draw.hwndItem);
  const LRESULT default_id =
      parent != nullptr ? SendMessageW(parent, DM_GETDEFID, 0, 0) : 0;
  const bool primary = !explicit_fill.has_value() &&
                       HIWORD(default_id) == DC_HASDEFID &&
                       LOWORD(default_id) == draw.CtlID;
  COLORREF fill = colors.fill;
  COLORREF text_color = colors.text;
  COLORREF outline = colors.border;
  if (disabled) {
    outline = colors.divider;
    text_color = colors.disabled;
  } else if (primary) {
    fill = pressed ? blend_color(colors.accent, colors.fill, 60)
           : hot   ? blend_color(colors.accent, colors.text, 40)
                   : colors.accent;
    outline = fill;
    text_color = colors.on_accent;
  } else if (pressed) {
    fill = colors.pressed;
  } else if (hot) {
    fill = colors.hover;
  }
  RECT parent_fill = draw.rcItem;
  fill_solid_rect(draw.hDC, parent_fill,
                  native_chrome_palette().menu.fill);
  const int radius = MulDiv(8, static_cast<int>(dpi), 96);
  const HBRUSH brush = CreateSolidBrush(fill);
  const HPEN pen = CreatePen(PS_SOLID, 1, outline);
  const HGDIOBJ previous_brush = SelectObject(draw.hDC, brush);
  const HGDIOBJ previous_pen = SelectObject(draw.hDC, pen);
  RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
            draw.rcItem.bottom, radius, radius);
  SelectObject(draw.hDC, previous_pen);
  SelectObject(draw.hDC, previous_brush);
  DeleteObject(pen);
  DeleteObject(brush);

  RECT text_rect = draw.rcItem;
  UINT text_format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  if (explicit_fill.has_value()) {
    const int item_height =
        static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
    const int swatch_size = std::min(MulDiv(16, static_cast<int>(dpi), 96),
                                     std::max(8, item_height - 10));
    const int swatch_top =
        draw.rcItem.top + (item_height - swatch_size) / 2;
    const int inset = MulDiv(8, static_cast<int>(dpi), 96);
    RECT swatch{draw.rcItem.left + inset, swatch_top,
                draw.rcItem.left + inset + swatch_size,
                swatch_top + swatch_size};
    fill_solid_rect(draw.hDC, swatch, *explicit_fill);
    frame_solid_rect(draw.hDC, swatch,
                     disabled ? colors.disabled : colors.border);
    text_rect.left = swatch.right + inset;
    text_rect.right -= inset / 2;
    text_format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  }
  if ((draw.itemState & ODS_NOACCEL) != 0)
    text_format |= DT_HIDEPREFIX;

  std::array<wchar_t, 160> label{};
  GetWindowTextW(draw.hwndItem, label.data(), static_cast<int>(label.size()));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, text_color);
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font);
  DrawTextW(draw.hDC, label.data(), -1, &text_rect, text_format);
  SelectObject(draw.hDC, previous_font);
  if ((draw.itemState & ODS_FOCUS) != 0 &&
      (draw.itemState & ODS_NOFOCUSRECT) == 0) {
    RECT focus = draw.rcItem;
    const HPEN ring = CreatePen(PS_SOLID | PS_INSIDEFRAME,
                                std::max(2, MulDiv(2, static_cast<int>(dpi), 96)),
                                primary ? colors.text : colors.accent);
    const HGDIOBJ old_pen = SelectObject(draw.hDC, ring);
    const HGDIOBJ old_brush = SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
    RoundRect(draw.hDC, focus.left, focus.top, focus.right, focus.bottom,
              radius, radius);
    SelectObject(draw.hDC, old_brush);
    SelectObject(draw.hDC, old_pen);
    DeleteObject(ring);
  }
}

} // namespace layerc::platform
