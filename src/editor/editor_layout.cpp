#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
void EditorWindow::layout() {
  if (shell_.window() == nullptr)
    return;
  RECT client{};
  GetClientRect(shell_.window(), &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const int layout_dpi = geometry_dpi(shell_.window(), 1080, 700);
  const auto scaled = [layout_dpi](const int value) {
    return dpi_scaled(value, layout_dpi);
  };

  const int margin = scaled(12);
  const int gap = scaled(14);
  const int section_top = scaled(12);
  const int heading_height = scaled(18);
  const int row_height = scaled(24);
  const int status_top = height - scaled(27);
  const int sidebar_width = std::clamp(width / 5, scaled(180), scaled(270));
  const int content_left = margin + sidebar_width + scaled(18);
  const int right_width =
      std::clamp(width * 30 / 100, scaled(285), scaled(420));
  const int right_left = width - margin - right_width;
  const int content_right = right_left - scaled(18);
  const int content_width = std::max(1, content_right - content_left);

  const auto move = [&](const int id, const int x, const int y,
                        const int control_width, const int control_height) {
    move_control(shell_.window(), id, x, y, control_width, control_height);
  };
  constexpr std::array advanced_controls{
      IDC_PROFILE_ID_LABEL,
      IDC_PROFILE_ID,
      IDC_APPLICATION_EXACT_PATH,
      IDC_CAPTURE_HID,
      IDC_VISUAL_ISOLATED,
      IDC_COOLDOWN_ID_LABEL,
      IDC_COOLDOWN_ID,
      IDC_COOLDOWN_CHARGES_LABEL,
      IDC_COOLDOWN_CHARGES,
      IDC_COOLDOWN_RETRIGGER_LABEL,
      IDC_COOLDOWN_RETRIGGER,
      IDC_COOLDOWN_RECOVERY_LABEL,
      IDC_COOLDOWN_RECOVERY,
      IDC_COOLDOWN_GROUPS_LABEL,
      IDC_COOLDOWN_GROUPS,
  };
  for (const int id : advanced_controls) {
    if (const HWND control = GetDlgItem(shell_.window(), id); control != nullptr)
      ShowWindow(control, shell_.advanced_visible() ? SW_SHOWNA : SW_HIDE);
  }

  move(IDC_SAVED_PROFILES_LABEL, margin, section_top, sidebar_width,
       heading_height);
  const int sidebar_list_top = section_top + heading_height + scaled(4);
  move(IDC_PROFILE_LIST, margin, sidebar_list_top, sidebar_width,
       std::max(scaled(120), status_top - sidebar_list_top - scaled(8)));

  move(IDC_PROFILE_SECTION_LABEL, content_left, section_top, content_width,
       heading_height);
  move(IDC_ADVANCED, content_right - scaled(104), section_top - scaled(2),
       scaled(104), scaled(24));
  const int field_label_width = scaled(86);
  const int field_left = content_left + field_label_width + scaled(8);
  const int profile_name_top = section_top + heading_height + scaled(5);
  move(IDC_PROFILE_NAME_LABEL, content_left, profile_name_top + scaled(3),
       field_label_width, heading_height);
  move(IDC_PROFILE_NAME, field_left, profile_name_top,
       content_right - field_left, row_height);

  int profile_row_top = profile_name_top + scaled(29);
  if (shell_.advanced_visible()) {
    move(IDC_PROFILE_ID_LABEL, content_left, profile_row_top + scaled(3),
         field_label_width, heading_height);
    move(IDC_PROFILE_ID, field_left, profile_row_top,
         content_right - field_left, row_height);
    profile_row_top += scaled(29);
  }
  const int choose_width = scaled(164);
  move(IDC_APPLICATION_LABEL, content_left, profile_row_top + scaled(3),
       field_label_width, heading_height);
  move(IDC_APPLICATION_PATH, field_left, profile_row_top,
       content_right - choose_width - scaled(7) - field_left, row_height);
  move(IDC_CHOOSE_APPLICATION, content_right - choose_width, profile_row_top,
       choose_width, row_height);
  profile_row_top += scaled(29);
  if (shell_.advanced_visible()) {
    move(IDC_APPLICATION_EXACT_PATH, field_left, profile_row_top,
         content_right - field_left, scaled(24));
    profile_row_top += scaled(29);
  }

  const int cooldown_heading_top = profile_row_top + scaled(9);
  const int cooldown_list_top =
      cooldown_heading_top + heading_height + scaled(4);
  const int cooldown_button_width = scaled(64);
  const int cooldown_list_width =
      content_width - cooldown_button_width - scaled(8);
  const int cooldown_list_height = scaled(shell_.advanced_visible() ? 78 : 90);
  move(IDC_COOLDOWNS_LABEL, content_left, cooldown_heading_top, content_width,
       heading_height);
  move(IDC_COOLDOWN_LIST, content_left, cooldown_list_top,
       cooldown_list_width, cooldown_list_height);
  move(IDC_ADD_COOLDOWN, content_left + cooldown_list_width + scaled(8),
       cooldown_list_top, cooldown_button_width, scaled(27));
  move(IDC_REMOVE_COOLDOWN, content_left + cooldown_list_width + scaled(8),
       cooldown_list_top + scaled(33), cooldown_button_width, scaled(27));

  const int cooldown_name_top =
      cooldown_list_top + cooldown_list_height + scaled(7);
  const int name_label_width = scaled(48);
  const int duration_label_width = scaled(112);
  const int duration_field_width = scaled(72);
  const int duration_left = content_right - duration_field_width;
  move(IDC_COOLDOWN_NAME_LABEL, content_left,
       cooldown_name_top + scaled(3), name_label_width, heading_height);
  move(IDC_COOLDOWN_NAME, content_left + name_label_width,
       cooldown_name_top,
       duration_left - duration_label_width - scaled(8) -
           (content_left + name_label_width),
       row_height);
  move(IDC_COOLDOWN_DURATION_LABEL,
       duration_left - duration_label_width, cooldown_name_top + scaled(3),
       duration_label_width, heading_height);
  move(IDC_COOLDOWN_DURATION, duration_left, cooldown_name_top,
       duration_field_width, row_height);

  const int binding_top = cooldown_name_top + scaled(31);
  const int binding_label_width = scaled(58);
  const int capture_width = scaled(104);
  const int hid_width = shell_.advanced_visible() ? scaled(66) : 0;
  const int binding_buttons_width =
      capture_width + (shell_.advanced_visible() ? scaled(7) + hid_width : 0);
  move(IDC_COOLDOWN_BINDING_LABEL, content_left, binding_top + scaled(3),
       binding_label_width, heading_height);
  move(IDC_COOLDOWN_BINDING, content_left + binding_label_width, binding_top,
       content_width - binding_label_width - binding_buttons_width -
           scaled(8),
       row_height);
  move(IDC_CAPTURE_BINDING, content_right - binding_buttons_width, binding_top,
       capture_width, row_height);
  if (shell_.advanced_visible())
    move(IDC_CAPTURE_HID, content_right - hid_width, binding_top, hid_width,
         row_height);

  int bars_heading_top = binding_top + row_height + scaled(18);
  if (shell_.advanced_visible()) {
    const int technical_top = binding_top + scaled(31);
    const int technical_label_width = scaled(78);
    const int charges_label_width = scaled(60);
    const int charges_width = scaled(54);
    move(IDC_COOLDOWN_ID_LABEL, content_left, technical_top + scaled(3),
         technical_label_width, heading_height);
    move(IDC_COOLDOWN_ID, content_left + technical_label_width, technical_top,
         content_width - technical_label_width - charges_label_width -
             charges_width - scaled(8),
         row_height);
    move(IDC_COOLDOWN_CHARGES_LABEL,
         content_right - charges_label_width - charges_width,
         technical_top + scaled(3), charges_label_width, heading_height);
    move(IDC_COOLDOWN_CHARGES, content_right - charges_width, technical_top,
         charges_width, row_height);

    const int policy_top = technical_top + scaled(31);
    const int half = (content_width - scaled(10)) / 2;
    const int policy_label_width = scaled(62);
    move(IDC_COOLDOWN_RETRIGGER_LABEL, content_left,
         policy_top + scaled(3), policy_label_width, heading_height);
    move(IDC_COOLDOWN_RETRIGGER, content_left + policy_label_width, policy_top,
         half - policy_label_width, scaled(90));
    const int recovery_left = content_left + half + scaled(10);
    move(IDC_COOLDOWN_RECOVERY_LABEL, recovery_left,
         policy_top + scaled(3), policy_label_width, heading_height);
    move(IDC_COOLDOWN_RECOVERY, recovery_left + policy_label_width, policy_top,
         content_right - recovery_left - policy_label_width, scaled(100));

    const int groups_top = policy_top + scaled(31);
    move(IDC_COOLDOWN_GROUPS_LABEL, content_left, groups_top + scaled(3),
         binding_label_width, heading_height);
    move(IDC_COOLDOWN_GROUPS, content_left + binding_label_width, groups_top,
         content_width - binding_label_width, row_height);
    bars_heading_top = groups_top + row_height + scaled(18);
  }

  const int bars_list_top = bars_heading_top + heading_height + scaled(4);
  const int bar_list_width =
      std::clamp(content_width * 2 / 5, scaled(185), scaled(220));
  const int properties_left = content_left + bar_list_width + gap;
  const int properties_width = content_width - bar_list_width - gap;
  const bool compact_swatches = properties_width < scaled(360);
  const int bars_height = std::clamp(
      status_top - bars_list_top - scaled(40),
      scaled(compact_swatches ? 184 : 118),
      scaled(compact_swatches ? 196 : 150));
  move(IDC_BARS_LABEL, content_left, bars_heading_top, content_width,
       heading_height);
  move(IDC_BAR_LIST, content_left, bars_list_top, bar_list_width, bars_height);
  move(IDC_SELECTED_BAR_LABEL, properties_left, bars_list_top,
       properties_width, heading_height);

  const int dimensions_top = bars_list_top + scaled(27);
  const int dimension_half = (properties_width - scaled(8)) / 2;
  move(IDC_WIDTH_LABEL, properties_left, dimensions_top + scaled(3),
       scaled(40), heading_height);
  move(IDC_BAR_WIDTH, properties_left + scaled(44), dimensions_top,
       std::max(scaled(42), dimension_half - scaled(44)), row_height);
  const int height_left = properties_left + dimension_half + scaled(8);
  move(IDC_HEIGHT_LABEL, height_left, dimensions_top + scaled(3), scaled(42),
       heading_height);
  move(IDC_BAR_HEIGHT, height_left + scaled(46), dimensions_top,
       std::max(scaled(42), properties_width - dimension_half - scaled(54)),
       row_height);

  const int swatch_top = bars_list_top + scaled(58);
  const int swatch_gap = scaled(5);
  if (compact_swatches) {
    const int swatch_width = (properties_width - swatch_gap) / 2;
    move(IDC_BAR_FILL, properties_left, swatch_top, swatch_width, scaled(27));
    move(IDC_BAR_BACKGROUND, properties_left + swatch_width + swatch_gap,
         swatch_top, properties_width - swatch_width - swatch_gap, scaled(27));
    const int second_swatch_top = swatch_top + scaled(34);
    move(IDC_BAR_BORDER, properties_left, second_swatch_top, swatch_width,
         scaled(27));
    move(IDC_BAR_TEXT, properties_left + swatch_width + swatch_gap,
         second_swatch_top, properties_width - swatch_width - swatch_gap,
         scaled(27));
  } else {
    const int swatch_width = (properties_width - swatch_gap * 3) / 4;
    move(IDC_BAR_FILL, properties_left, swatch_top, swatch_width, scaled(27));
    move(IDC_BAR_BACKGROUND, properties_left + swatch_width + swatch_gap,
         swatch_top, swatch_width, scaled(27));
    move(IDC_BAR_BORDER, properties_left + (swatch_width + swatch_gap) * 2,
         swatch_top, swatch_width, scaled(27));
    move(IDC_BAR_TEXT, properties_left + (swatch_width + swatch_gap) * 3,
         swatch_top, properties_width - (swatch_width + swatch_gap) * 3,
         scaled(27));
  }
  const int checkbox_top =
      swatch_top + scaled(compact_swatches ? 68 : 34);
  const int checkbox_width = (properties_width - scaled(8)) / 2;
  move(IDC_SHOW_TEXT, properties_left, checkbox_top, checkbox_width,
       scaled(25));
  move(IDC_HIDE_READY, properties_left + checkbox_width + scaled(8),
       checkbox_top, properties_width - checkbox_width - scaled(8),
       scaled(25));
  move(IDC_DIRECT_DRAG, properties_left, checkbox_top + scaled(29),
       properties_width, scaled(25));
  move(IDC_POSITION_STATUS, content_left,
       bars_list_top + bars_height + scaled(6), content_width, scaled(26));

  move(IDC_VISUAL_SECTION_LABEL, right_left, section_top, right_width,
       heading_height);
  const int visual_summary_top = section_top + heading_height + scaled(4);
  move(IDC_VISUAL_SUMMARY, right_left, visual_summary_top,
       right_width - scaled(90), scaled(22));
  move(IDC_VISUAL_ENABLED, right_left + right_width - scaled(84),
       visual_summary_top, scaled(84), scaled(22));
  const int visual_actions_top = visual_summary_top + scaled(28);
  move(IDC_CALIBRATE_VISUAL, right_left, visual_actions_top, scaled(112),
       scaled(27));
  move(IDC_REMOVE_VISUAL, right_left + scaled(120), visual_actions_top,
       scaled(78), scaled(27));
  if (shell_.advanced_visible())
    move(IDC_VISUAL_ISOLATED, right_left + scaled(206), visual_actions_top,
         right_width - scaled(206), scaled(27));
  const int visual_status_top = visual_actions_top + scaled(34);
  move(IDC_VISUAL_RUNTIME_STATUS, right_left, visual_status_top, right_width,
       scaled(36));

  const int preview_heading_top = visual_status_top + scaled(45);
  const int position_width = scaled(154);
  move(IDC_PREVIEW_LABEL, right_left, preview_heading_top,
       right_width - position_width - scaled(8), heading_height);
  move(IDC_POSITION_ON_WINDOW, right_left + right_width - position_width,
       preview_heading_top - scaled(4), position_width, scaled(27));
  const int preview_top = preview_heading_top + scaled(30);
  const int preview_height =
      std::clamp(right_width * 3 / 4, scaled(220), scaled(340));
  move(IDC_PREVIEW, right_left, preview_top, right_width,
       std::min(preview_height,
                std::max(scaled(120), status_top - preview_top - scaled(8))));

  move(IDC_STATUS, margin, status_top, width - margin * 2, scaled(19));
  ListView_SetColumnWidth(shell_.profile_list(), 0,
                          std::max(1, sidebar_width - scaled(8)));
  RedrawWindow(shell_.window(), nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

} // namespace layerc::editor
