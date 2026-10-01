#include "editor_window.hpp"
#include "dialog_layout.hpp"

#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <optional>
#include <commctrl.h>
#include <uxtheme.h>
#include <windows.h>

namespace layerc::editor {
namespace {

// Average character width and height of `font`, measured the way the dialog
// manager derives dialog base units.
std::optional<SIZE> font_base_units(const HWND window, const HFONT font) {
  const HDC dc = GetDC(window);
  if (dc == nullptr)
    return std::nullopt;
  const HGDIOBJ previous = SelectObject(dc, font);
  static constexpr wchar_t sample[] =
      L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  TEXTMETRICW metrics{};
  SIZE extent{};
  std::optional<SIZE> units;
  if (GetTextMetricsW(dc, &metrics) != FALSE &&
      GetTextExtentPoint32W(dc, sample, 52, &extent) != FALSE)
    units = SIZE{(extent.cx / 26 + 1) / 2, metrics.tmHeight};
  SelectObject(dc, previous);
  ReleaseDC(window, dc);
  return units;
}

// Fixed dialog templates are laid out in dialog units of their 9 pt template
// font, but render with the larger body font (14 px, times the text size).
// Scale the template once so its text fits; resizable dialogs lay out in
// code through window_base_dpi instead. Per-monitor DPI changes later rescale
// the dialog proportionally, so this runs only at creation.
void fit_dialog_to_font(const HWND dialog) {
  const auto style = GetWindowLongPtrW(dialog, GWL_STYLE);
  if ((style & WS_THICKFRAME) != 0 || (style & WS_CHILD) != 0)
    return;
  RECT template_units{0, 0, 4, 8};
  if (MapDialogRect(dialog, &template_units) == FALSE ||
      template_units.right <= 0 || template_units.bottom <= 0)
    return;
  const HFONT font =
      layerc::platform::create_native_dialog_font(GetDpiForWindow(dialog));
  if (font == nullptr)
    return;
  const auto units = font_base_units(dialog, font);
  DeleteObject(font);
  if (!units)
    return;
  const int scale = std::clamp(
      std::max(MulDiv(units->cx, 1000, template_units.right),
               MulDiv(units->cy, 1000, template_units.bottom)),
      1000, 2500);
  if (scale <= 1020)
    return;
  const auto scaled = [scale](const LONG value) {
    return MulDiv(static_cast<int>(value), scale, 1000);
  };
  // Buttons and checkboxes keep the one 30 px control height used by the
  // code-laid-out surfaces instead of growing with the template scale.
  const int control_height = dpi_scaled(30, window_base_dpi(dialog));
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    RECT bounds{};
    GetWindowRect(child, &bounds);
    const auto klass = layerc::platform::window_class(child);
    const auto button_type =
        GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK;
    if (klass == L"Button" && button_type != BS_GROUPBOX) {
      MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT *>(&bounds),
                      2);
      const int slot_top = scaled(bounds.top);
      const int slot_height =
          std::max(1, scaled(bounds.bottom - bounds.top));
      const int height = std::min(slot_height, control_height);
      MoveWindow(child, scaled(bounds.left),
                 slot_top + (slot_height - height) / 2,
                 std::max(1, scaled(bounds.right - bounds.left)), height,
                 FALSE);
      continue;
    }
    if (klass == L"ComboBox") {
      RECT dropped{};
      if (SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0,
                       reinterpret_cast<LPARAM>(&dropped)) != FALSE)
        bounds.bottom = std::max(bounds.bottom, dropped.bottom);
    }
    MapWindowPoints(HWND_DESKTOP, dialog, reinterpret_cast<POINT *>(&bounds), 2);
    MoveWindow(child, scaled(bounds.left), scaled(bounds.top),
               std::max(1, scaled(bounds.right - bounds.left)),
               std::max(1, scaled(bounds.bottom - bounds.top)), FALSE);
  }
  RECT client{};
  GetClientRect(dialog, &client);
  RECT frame{0, 0, scaled(client.right), scaled(client.bottom)};
  AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(style),
                           GetMenu(dialog) != nullptr,
                           static_cast<DWORD>(
                               GetWindowLongPtrW(dialog, GWL_EXSTYLE)),
                           GetDpiForWindow(dialog));
  SetWindowPos(dialog, nullptr, 0, 0, frame.right - frame.left,
               frame.bottom - frame.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Owned dialogs open centered on their owner, inside its monitor's work area.
void center_over_owner(const HWND dialog) {
  const HWND owner = GetWindow(dialog, GW_OWNER);
  if (owner == nullptr || IsIconic(owner) != FALSE)
    return;
  RECT owner_bounds{};
  RECT bounds{};
  GetWindowRect(owner, &owner_bounds);
  GetWindowRect(dialog, &bounds);
  const int width = bounds.right - bounds.left;
  const int height = bounds.bottom - bounds.top;
  int left = owner_bounds.left + (owner_bounds.right - owner_bounds.left - width) / 2;
  int top = owner_bounds.top + (owner_bounds.bottom - owner_bounds.top - height) / 2;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST),
                      &monitor) != FALSE) {
    const RECT &work = monitor.rcWork;
    left = std::max(static_cast<int>(work.left),
                    std::min(left, static_cast<int>(work.right) - width));
    top = std::max(static_cast<int>(work.top),
                   std::min(top, static_cast<int>(work.bottom) - height));
  }
  SetWindowPos(dialog, nullptr, left, top, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

void style_native_dialog_children(const HWND dialog, const HFONT font) {
  layerc::platform::style_native_dialog_children(dialog, font, true);
}

void apply_dialog_chrome(const HWND window, const bool capture_exclusion) {
  layerc::platform::set_dark_title_bar(window);
  // Windows 11: caption and border join the client surface (ignored on 10).
  const auto frame = layerc::platform::native_chrome_palette().menu;
  layerc::platform::set_window_frame_colors(window, frame.fill, frame.divider,
                                            frame.text);
  // Best-effort: capture exclusion is a no-op where the DWM API is missing.
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion);
  fit_dialog_to_font(window);
  center_over_owner(window);
}

void EditorWindow::style_dialog_children(const HWND dialog) const {
  style_native_dialog_children(dialog, shell_.font);
}

void EditorWindow::style_list_view(const HWND list,
                                   const bool hide_header) const {
  if (list == nullptr)
    return;
  auto style = GetWindowLongPtrW(list, GWL_STYLE);
  if (hide_header)
    style |= LVS_NOCOLUMNHEADER;
  SetWindowLongPtrW(list, GWL_STYLE, style);
  const auto list_colors =
      colors(list == shell_.profile_list
                 ? layerc::platform::EditorControlRole::sidebar
                 : layerc::platform::EditorControlRole::list);
  ListView_SetBkColor(list, list_colors.fill);
  ListView_SetTextBkColor(list, list_colors.fill);
  ListView_SetTextColor(list, list_colors.text);
  SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
  if (const HWND header = ListView_GetHeader(list); header != nullptr)
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
  layerc::platform::install_list_header_text(list);
  SetWindowPos(list, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
}

void EditorWindow::rebuild_profile_check_hit_images() {
  if (shell_.profile_list == nullptr)
    return;
  const auto metrics = profile_checkbox_metrics(GetDpiForWindow(shell_.profile_list));
  const HIMAGELIST images = ImageList_Create(
      metrics.image_size, metrics.image_size, ILC_COLOR32 | ILC_MASK, 2, 0);
  if (images == nullptr)
    return;

  // LVS_EX_CHECKBOXES uses the state-image dimensions for hit testing even
  // when LVS_OWNERDRAWFIXED paints the row. Keep transparent state images for
  // that geometry; the single paint_checkbox path owns all visible pixels.
  const HBITMAP transparent =
      CreateBitmap(metrics.image_size, metrics.image_size, 1, 32, nullptr);
  if (transparent == nullptr ||
      ImageList_AddMasked(images, transparent, RGB(0, 0, 0)) < 0 ||
      ImageList_AddMasked(images, transparent, RGB(0, 0, 0)) < 0) {
    if (transparent != nullptr)
      DeleteObject(transparent);
    ImageList_Destroy(images);
    return;
  }
  DeleteObject(transparent);

  (void)ListView_SetImageList(shell_.profile_list, images, LVSIL_STATE);
  if (shell_.profile_check_hit_images != nullptr)
    ImageList_Destroy(shell_.profile_check_hit_images);
  shell_.profile_check_hit_images = images;
}

void EditorWindow::rebuild_brushes() {
  for (auto &brush_value : appearance_editor_.brushes()) {
    if (brush_value != nullptr)
      DeleteObject(brush_value);
    brush_value = nullptr;
  }
  for (std::uint32_t index = 0;
       index <
       static_cast<std::uint32_t>(layerc::platform::EditorControlRole::count);
       ++index) {
    const auto role = static_cast<layerc::platform::EditorControlRole>(index);
    appearance_editor_.brushes()[index] = CreateSolidBrush(colors(role).fill);
  }
}

void EditorWindow::publish_chrome_palette() const {
  layerc::platform::set_native_chrome_palette(
      {.frame = colors(layerc::platform::EditorControlRole::edit),
       .menu = colors(layerc::platform::EditorControlRole::toolbar),
       .button = colors(layerc::platform::EditorControlRole::button)});
}

void EditorWindow::refresh_theme() {
  publish_chrome_palette();
  rebuild_brushes();
  style_list_view(shell_.profile_list, true);
  rebuild_profile_check_hit_images();
  style_list_view(task_picker_.list, false);
  const auto frame = colors(layerc::platform::EditorControlRole::toolbar);
  for (const HWND window : {shell_.window, task_picker_.window}) {
    if (window != nullptr && IsWindow(window))
      layerc::platform::set_window_frame_colors(window, frame.fill,
                                                frame.divider, frame.text);
  }
  if (shell_.window != nullptr) {
    RedrawWindow(shell_.window, nullptr, nullptr,
                 RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    DrawMenuBar(shell_.window);
    InvalidateRect(shell_.window, nullptr, TRUE);
    EnumChildWindows(
        shell_.window,
        [](HWND child, LPARAM) -> BOOL {
          InvalidateRect(child, nullptr, TRUE);
          return TRUE;
        },
        0);
  }
  if (task_picker_.window != nullptr)
    InvalidateRect(task_picker_.window, nullptr, TRUE);
}

} // namespace layerc::editor
