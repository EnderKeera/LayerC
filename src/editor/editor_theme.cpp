#include "editor_window.hpp"

#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <commctrl.h>
#include <uxtheme.h>
#include <windows.h>

namespace layerc::editor {

ProfileCheckboxMetrics profile_checkbox_metrics(const UINT source_dpi) {
  const int dpi = source_dpi == 0 ? 96 : static_cast<int>(source_dpi);
  return {
      .image_size = std::max(1, MulDiv(16, dpi, 96)),
      .inset = std::max(1, MulDiv(2, dpi, 96)),
      .pen_width = std::max(1, MulDiv(2, dpi, 96)),
  };
}

void set_owner_draw_checkbox(const HWND control, const bool checked) {
  if (control == nullptr)
    return;
  SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  InvalidateRect(control, nullptr, TRUE);
}

bool owner_draw_checkbox_checked(const HWND control) {
  return control != nullptr &&
         SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

COLORREF color_ref(const std::uint32_t color) {
  return layerc::platform::native_color_ref(color);
}

std::uint32_t rgb_value(const COLORREF color) {
  return layerc::platform::native_rgb_value(color);
}

void style_native_dialog_children(const HWND dialog, const HFONT font) {
  layerc::platform::style_native_dialog_children(dialog, font, true);
}

void EditorWindow::style_dialog_children(const HWND dialog) const {
  style_native_dialog_children(dialog, font_);
}

void EditorWindow::style_list_view(const HWND list,
                                   const bool hide_header) const {
  if (list == nullptr)
    return;
  auto style = GetWindowLongPtrW(list, GWL_STYLE);
  if (hide_header)
    style |= LVS_NOCOLUMNHEADER;
  SetWindowLongPtrW(list, GWL_STYLE, style);
  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  ListView_SetBkColor(list, list_colors.fill);
  ListView_SetTextBkColor(list, list_colors.fill);
  ListView_SetTextColor(list, list_colors.text);
  SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
  if (const HWND header = ListView_GetHeader(list); header != nullptr)
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
  SetWindowPos(list, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);
}

void EditorWindow::rebuild_profile_check_hit_images() {
  if (profile_list_ == nullptr)
    return;
  const auto metrics = profile_checkbox_metrics(GetDpiForWindow(profile_list_));
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

  (void)ListView_SetImageList(profile_list_, images, LVSIL_STATE);
  if (profile_check_hit_images_ != nullptr)
    ImageList_Destroy(profile_check_hit_images_);
  profile_check_hit_images_ = images;
}

void EditorWindow::rebuild_brushes() {
  for (auto &brush_value : brushes_) {
    if (brush_value != nullptr)
      DeleteObject(brush_value);
    brush_value = nullptr;
  }
  for (std::uint32_t index = 0;
       index <
       static_cast<std::uint32_t>(layerc::platform::EditorControlRole::count);
       ++index) {
    const auto role = static_cast<layerc::platform::EditorControlRole>(index);
    brushes_[index] = CreateSolidBrush(colors(role).fill);
  }
}

void EditorWindow::refresh_theme() {
  rebuild_brushes();
  style_list_view(profile_list_, true);
  rebuild_profile_check_hit_images();
  style_list_view(task_list_, false);
  if (dialog_ != nullptr) {
    DrawMenuBar(dialog_);
    InvalidateRect(dialog_, nullptr, TRUE);
    EnumChildWindows(
        dialog_,
        [](HWND child, LPARAM) -> BOOL {
          InvalidateRect(child, nullptr, TRUE);
          return TRUE;
        },
        0);
  }
  if (task_dialog_ != nullptr)
    InvalidateRect(task_dialog_, nullptr, TRUE);
}

} // namespace layerc::editor
