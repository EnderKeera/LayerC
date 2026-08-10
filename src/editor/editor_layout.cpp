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
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const UINT window_dpi = GetDpiForWindow(dialog_);
  const int dpi = window_dpi == 0 ? 96 : static_cast<int>(window_dpi);
  // Keep the three-column editor usable on compact/high-DPI work areas by
  // reducing geometry scale only as far as necessary. The font remains at the
  // monitor DPI, so text stays legible while controls avoid overlapping.
  const int geometry_dpi = std::max(
      80, std::min({dpi, MulDiv(width, 96, 1080),
                    MulDiv(height, 96, 700)}));
  const auto scaled = [geometry_dpi](const int value) {
    return MulDiv(value, geometry_dpi, 96);
  };

  const int margin = scaled(12);
  const int gap = scaled(14);
  const int toolbar_top = scaled(8);
  const int toolbar_height = scaled(28);
  const int section_top = scaled(50);
  const int heading_height = scaled(18);
  const int row_height = scaled(24);
  const int status_top = height - scaled(27);
  const int sidebar_actions_top = status_top - scaled(36);
  const int sidebar_width =
      std::clamp(width / 5, scaled(180), scaled(270));
  const int content_left = margin + sidebar_width + scaled(18);
  const int preview_width =
      std::clamp(width * 32 / 100, scaled(260), scaled(470));
  const int preview_left = width - margin - preview_width;
  const int content_right = preview_left - scaled(18);
  const int content_width = std::max(1, content_right - content_left);

  const auto move = [&](const int id, const int x, const int y,
                        const int control_width, const int control_height) {
    if (const HWND control = GetDlgItem(dialog_, id); control != nullptr)
      MoveWindow(control, x, y, std::max(1, control_width),
                 std::max(1, control_height), TRUE);
  };

  int toolbar_x = scaled(10);
  auto toolbar_button = [&](const int id, const int button_width) {
    const int scaled_width = scaled(button_width);
    move(id, toolbar_x, toolbar_top, scaled_width, toolbar_height);
    toolbar_x += scaled_width + scaled(6);
  };
  toolbar_button(IDC_FILE_MENU, 50);

  move(IDC_SAVED_PROFILES_LABEL, margin, section_top, sidebar_width, heading_height);
  const int sidebar_list_top = section_top + heading_height + scaled(4);
  move(IDC_PROFILE_LIST, margin, sidebar_list_top, sidebar_width,
       std::max(scaled(120),
                sidebar_actions_top - sidebar_list_top - scaled(8)));
  const int sidebar_action_gap = scaled(6);
  const int sidebar_action_width =
      std::max(1, (sidebar_width - sidebar_action_gap) / 2);
  move(IDC_ATTACH_LEFT, margin, sidebar_actions_top, sidebar_action_width,
       scaled(27));
  move(IDC_SHOW_TASKS_LEFT,
       margin + sidebar_action_width + sidebar_action_gap,
       sidebar_actions_top,
       sidebar_width - sidebar_action_width - sidebar_action_gap, scaled(27));

  move(IDC_PROFILE_SECTION_LABEL, content_left, section_top,
       content_width, heading_height);
  const int field_label_width = scaled(86);
  const int field_left = content_left + field_label_width + scaled(8);
  const int field_width = content_width - field_label_width - scaled(8);
  const int profile_name_top = section_top + heading_height + scaled(5);
  const int profile_id_top = profile_name_top + scaled(29);
  const int application_top = profile_id_top + scaled(29);
  move(IDC_PROFILE_NAME_LABEL, content_left, profile_name_top + scaled(3),
       field_label_width, heading_height);
  move(IDC_PROFILE_NAME, field_left, profile_name_top, field_width, row_height);
  move(IDC_PROFILE_ID_LABEL, content_left, profile_id_top + scaled(3),
       field_label_width, heading_height);
  move(IDC_PROFILE_ID, field_left, profile_id_top, field_width, row_height);
  move(IDC_APPLICATION_LABEL, content_left, application_top + scaled(3),
       field_label_width, heading_height);
  move(IDC_APPLICATION_PATH, field_left, application_top, field_width, row_height);

  const int cooldown_heading_top = application_top + row_height + scaled(17);
  const int cooldown_list_top = cooldown_heading_top + heading_height + scaled(4);
  const int cooldown_button_width = scaled(64);
  const int cooldown_list_width =
      content_width - cooldown_button_width - scaled(8);
  move(IDC_COOLDOWNS_LABEL, content_left, cooldown_heading_top,
       content_width, heading_height);
  move(IDC_COOLDOWN_LIST, content_left, cooldown_list_top,
       cooldown_list_width, scaled(112));
  move(IDC_ADD_COOLDOWN, content_left + cooldown_list_width + scaled(8),
       cooldown_list_top, cooldown_button_width, scaled(27));
  move(IDC_REMOVE_COOLDOWN, content_left + cooldown_list_width + scaled(8),
       cooldown_list_top + scaled(33), cooldown_button_width, scaled(27));
  const int binding_top = cooldown_list_top + scaled(119);
  const int binding_label_width = scaled(58);
  const int capture_button_width = scaled(108);
  move(IDC_COOLDOWN_BINDING_LABEL, content_left, binding_top + scaled(3),
       binding_label_width, heading_height);
  move(IDC_COOLDOWN_BINDING, content_left + binding_label_width + scaled(6),
       binding_top,
       content_width - binding_label_width - capture_button_width - scaled(20),
       row_height);
  move(IDC_CAPTURE_BINDING, content_right - capture_button_width, binding_top,
       capture_button_width, row_height);

  const int bars_heading_top = binding_top + row_height + scaled(18);
  const int bars_list_top = bars_heading_top + heading_height + scaled(4);
  const int bar_list_width =
      std::clamp(content_width * 2 / 5, scaled(185), scaled(215));
  const int properties_left = content_left + bar_list_width + gap;
  const int properties_width = content_width - bar_list_width - gap;
  move(IDC_BARS_LABEL, content_left, bars_heading_top,
       content_width, heading_height);
  move(IDC_BAR_LIST, content_left, bars_list_top, bar_list_width, scaled(134));
  move(IDC_SELECTED_BAR_LABEL, properties_left, bars_list_top,
       properties_width, heading_height);

  const int dimensions_top = bars_list_top + scaled(27);
  const int dimension_half = (properties_width - scaled(8)) / 2;
  move(IDC_WIDTH_LABEL, properties_left, dimensions_top + scaled(3),
       scaled(40), heading_height);
  move(IDC_BAR_WIDTH, properties_left + scaled(44), dimensions_top,
       std::max(scaled(46), dimension_half - scaled(44)), row_height);
  const int height_left = properties_left + dimension_half + scaled(8);
  move(IDC_HEIGHT_LABEL, height_left, dimensions_top + scaled(3),
       scaled(42), heading_height);
  move(IDC_BAR_HEIGHT, height_left + scaled(46), dimensions_top,
       std::max(scaled(46), properties_width - dimension_half - scaled(54)),
       row_height);

  const int color_button_width = (properties_width - scaled(8)) / 2;
  const int fill_button_width = std::min(color_button_width,
      std::max(scaled(110), (properties_width - scaled(8)) * 38 / 100));
  const int first_color_top = bars_list_top + scaled(58);
  move(IDC_BAR_FILL, properties_left, first_color_top,
       fill_button_width, scaled(27));
  move(IDC_BAR_BACKGROUND, properties_left + fill_button_width + scaled(8),
       first_color_top, properties_width - fill_button_width - scaled(8),
       scaled(27));
  move(IDC_BAR_BORDER, properties_left, first_color_top + scaled(33),
       color_button_width, scaled(27));
  move(IDC_BAR_TEXT, properties_left + color_button_width + scaled(8),
       first_color_top + scaled(33),
       properties_width - color_button_width - scaled(8), scaled(27));
  const int checkbox_width = (properties_width - scaled(8)) / 2;
  move(IDC_SHOW_TEXT, properties_left, first_color_top + scaled(67),
       checkbox_width, scaled(25));
  move(IDC_HIDE_READY, properties_left + checkbox_width + scaled(8),
       first_color_top + scaled(67),
       properties_width - checkbox_width - scaled(8), scaled(25));
  move(IDC_DIRECT_DRAG, properties_left, first_color_top + scaled(99),
       properties_width, scaled(25));
  // Anchor the visual strip above the bottom status line. A bars-relative
  // position overlapped the status area at the minimum window height.
  const int visual_heading_top = status_top - scaled(126);
  move(IDC_POSITION_STATUS, content_left,
       std::min(bars_list_top + scaled(190),
                visual_heading_top - scaled(30)),
       content_width, scaled(20));

  move(IDC_VISUAL_SECTION_LABEL, content_left, visual_heading_top,
       content_width, heading_height);
  const int visual_summary_top = visual_heading_top + scaled(22);
  move(IDC_VISUAL_SUMMARY, content_left, visual_summary_top,
       content_width - scaled(90), scaled(22));
  move(IDC_VISUAL_ENABLED, content_right - scaled(82), visual_summary_top,
       scaled(82), scaled(22));
  const int visual_actions_top = visual_summary_top + scaled(28);
  move(IDC_CALIBRATE_VISUAL, content_left, visual_actions_top,
       scaled(112), scaled(27));
  move(IDC_REMOVE_VISUAL, content_left + scaled(120), visual_actions_top,
       scaled(78), scaled(27));
  move(IDC_VISUAL_RUNTIME_STATUS, content_left,
       visual_actions_top + scaled(34), content_width, scaled(34));

  move(IDC_PREVIEW_LABEL, preview_left, section_top,
       preview_width, heading_height);
  const int preview_top = section_top + heading_height + scaled(4);
  move(IDC_PREVIEW, preview_left, preview_top, preview_width,
       std::max(scaled(240), status_top - preview_top - scaled(10)));
  move(IDC_STATUS, margin, status_top, width - margin * 2, scaled(19));
  ListView_SetColumnWidth(profile_list_, 0,
                          std::max(1, sidebar_width - scaled(8)));
}

} // namespace layerc::editor
