#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
void EditorWindow::layout() {
  if (dialog_ == nullptr) return;
  RECT client{};
  GetClientRect(dialog_, &client);
  const int width = std::max(980, static_cast<int>(client.right - client.left));
  const int height = std::max(620, static_cast<int>(client.bottom - client.top));

  constexpr int margin = 12;
  constexpr int gap = 14;
  constexpr int toolbar_top = 8;
  constexpr int toolbar_height = 28;
  constexpr int section_top = 50;
  constexpr int heading_height = 18;
  constexpr int row_height = 24;
  const int status_top = height - 27;
  const int sidebar_actions_top = status_top - 36;
  const int sidebar_width = std::clamp(width / 5, 220, 270);
  const int content_left = margin + sidebar_width + 18;
  const int preview_width = std::clamp(width * 32 / 100, 330, 470);
  const int preview_left = width - margin - preview_width;
  const int content_right = preview_left - 18;
  const int content_width = std::max(360, content_right - content_left);

  const auto move = [&](const int id, const int x, const int y,
                        const int control_width, const int control_height) {
    if (const HWND control = GetDlgItem(dialog_, id); control != nullptr)
      MoveWindow(control, x, y, std::max(1, control_width),
                 std::max(1, control_height), TRUE);
  };

  int toolbar_x = 10;
  auto toolbar_button = [&](const int id, const int button_width) {
    move(id, toolbar_x, toolbar_top, button_width, toolbar_height);
    toolbar_x += button_width + 6;
  };
  toolbar_button(IDC_FILE_MENU, 50);

  move(IDC_SAVED_PROFILES_LABEL, margin, section_top, sidebar_width, heading_height);
  const int sidebar_list_top = section_top + heading_height + 4;
  move(IDC_PROFILE_LIST, margin, sidebar_list_top, sidebar_width,
       std::max(120, sidebar_actions_top - sidebar_list_top - 8));
  move(IDC_ATTACH_LEFT, margin, sidebar_actions_top, 82, 27);
  move(IDC_SHOW_TASKS_LEFT, margin + 88, sidebar_actions_top,
       sidebar_width - 88, 27);

  move(IDC_PROFILE_SECTION_LABEL, content_left, section_top,
       content_width, heading_height);
  constexpr int field_label_width = 86;
  const int field_left = content_left + field_label_width + 8;
  const int field_width = content_width - field_label_width - 8;
  const int profile_name_top = section_top + heading_height + 5;
  const int profile_id_top = profile_name_top + 29;
  const int application_top = profile_id_top + 29;
  move(IDC_PROFILE_NAME_LABEL, content_left, profile_name_top + 3,
       field_label_width, heading_height);
  move(IDC_PROFILE_NAME, field_left, profile_name_top, field_width, row_height);
  move(IDC_PROFILE_ID_LABEL, content_left, profile_id_top + 3,
       field_label_width, heading_height);
  move(IDC_PROFILE_ID, field_left, profile_id_top, field_width, row_height);
  move(IDC_APPLICATION_LABEL, content_left, application_top + 3,
       field_label_width, heading_height);
  move(IDC_APPLICATION_PATH, field_left, application_top, field_width, row_height);

  const int cooldown_heading_top = application_top + row_height + 17;
  const int cooldown_list_top = cooldown_heading_top + heading_height + 4;
  const int cooldown_button_width = 64;
  const int cooldown_list_width = content_width - cooldown_button_width - 8;
  move(IDC_COOLDOWNS_LABEL, content_left, cooldown_heading_top,
       content_width, heading_height);
  move(IDC_COOLDOWN_LIST, content_left, cooldown_list_top,
       cooldown_list_width, 112);
  move(IDC_ADD_COOLDOWN, content_left + cooldown_list_width + 8,
       cooldown_list_top, cooldown_button_width, 27);
  move(IDC_REMOVE_COOLDOWN, content_left + cooldown_list_width + 8,
       cooldown_list_top + 33, cooldown_button_width, 27);
  const int binding_top = cooldown_list_top + 119;
  constexpr int binding_label_width = 58;
  constexpr int capture_button_width = 108;
  move(IDC_COOLDOWN_BINDING_LABEL, content_left, binding_top + 3,
       binding_label_width, heading_height);
  move(IDC_COOLDOWN_BINDING, content_left + binding_label_width + 6,
       binding_top, content_width - binding_label_width - capture_button_width - 20,
       row_height);
  move(IDC_CAPTURE_BINDING, content_right - capture_button_width, binding_top,
       capture_button_width, row_height);

  const int bars_heading_top = binding_top + row_height + 18;
  const int bars_list_top = bars_heading_top + heading_height + 4;
  const int bar_list_width = std::clamp(content_width * 2 / 5, 185, 215);
  const int properties_left = content_left + bar_list_width + gap;
  const int properties_width = content_width - bar_list_width - gap;
  move(IDC_BARS_LABEL, content_left, bars_heading_top,
       content_width, heading_height);
  move(IDC_BAR_LIST, content_left, bars_list_top, bar_list_width, 134);
  move(IDC_SELECTED_BAR_LABEL, properties_left, bars_list_top,
       properties_width, heading_height);

  const int dimensions_top = bars_list_top + 27;
  const int dimension_half = (properties_width - 8) / 2;
  move(IDC_WIDTH_LABEL, properties_left, dimensions_top + 3, 40, heading_height);
  move(IDC_BAR_WIDTH, properties_left + 44, dimensions_top,
       std::max(46, dimension_half - 44), row_height);
  const int height_left = properties_left + dimension_half + 8;
  move(IDC_HEIGHT_LABEL, height_left, dimensions_top + 3, 42, heading_height);
  move(IDC_BAR_HEIGHT, height_left + 46, dimensions_top,
       std::max(46, properties_width - dimension_half - 54), row_height);

  const int color_button_width = (properties_width - 8) / 2;
  const int fill_button_width = std::min(color_button_width,
      std::max(110, (properties_width - 8) * 38 / 100));
  const int first_color_top = bars_list_top + 58;
  move(IDC_BAR_FILL, properties_left, first_color_top,
       fill_button_width, 27);
  move(IDC_BAR_BACKGROUND, properties_left + fill_button_width + 8,
       first_color_top, properties_width - fill_button_width - 8, 27);
  move(IDC_BAR_BORDER, properties_left, first_color_top + 33,
       color_button_width, 27);
  move(IDC_BAR_TEXT, properties_left + color_button_width + 8,
       first_color_top + 33, properties_width - color_button_width - 8, 27);
  const int checkbox_width = (properties_width - 8) / 2;
  move(IDC_SHOW_TEXT, properties_left, first_color_top + 67,
       checkbox_width, 25);
  move(IDC_HIDE_READY, properties_left + checkbox_width + 8,
       first_color_top + 67, properties_width - checkbox_width - 8, 25);
  move(IDC_POSITION_STATUS, content_left, bars_list_top + 153,
       content_width, 20);

  move(IDC_PREVIEW_LABEL, preview_left, section_top,
       preview_width, heading_height);
  const int preview_top = section_top + heading_height + 4;
  move(IDC_PREVIEW, preview_left, preview_top, preview_width,
       std::max(240, status_top - preview_top - 10));
  move(IDC_STATUS, margin, status_top, width - margin * 2, 19);
  ListView_SetColumnWidth(profile_list_, 0, sidebar_width - 8);
}

} // namespace layerc::editor
