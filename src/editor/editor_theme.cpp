#include "editor_window.hpp"
#include "dialog_layout.hpp"

#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <commctrl.h>
#include <uxtheme.h>
#include <windows.h>

namespace layerc::editor {

void style_native_dialog_children(const HWND dialog, const HFONT font) {
  layerc::platform::style_native_dialog_children(dialog, font, true);
}

void apply_dialog_chrome(const HWND window, const bool capture_exclusion) {
  layerc::platform::set_dark_title_bar(window);
  // Best-effort: capture exclusion is a no-op where the DWM API is missing.
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion);
}

void EditorWindow::style_dialog_children(const HWND dialog) const {
  style_native_dialog_children(dialog, shell_.font());
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
  if (shell_.profile_list() == nullptr)
    return;
  const auto metrics = profile_checkbox_metrics(GetDpiForWindow(shell_.profile_list()));
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

  (void)ListView_SetImageList(shell_.profile_list(), images, LVSIL_STATE);
  if (shell_.profile_check_hit_images() != nullptr)
    ImageList_Destroy(shell_.profile_check_hit_images());
  shell_.profile_check_hit_images() = images;
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

void EditorWindow::refresh_theme() {
  rebuild_brushes();
  style_list_view(shell_.profile_list(), true);
  rebuild_profile_check_hit_images();
  style_list_view(task_picker_.list(), false);
  if (shell_.window() != nullptr) {
    DrawMenuBar(shell_.window());
    InvalidateRect(shell_.window(), nullptr, TRUE);
    EnumChildWindows(
        shell_.window(),
        [](HWND child, LPARAM) -> BOOL {
          InvalidateRect(child, nullptr, TRUE);
          return TRUE;
        },
        0);
  }
  if (task_picker_.window() != nullptr)
    InvalidateRect(task_picker_.window(), nullptr, TRUE);
}

} // namespace layerc::editor
