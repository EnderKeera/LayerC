#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>

#include <windows.h>

namespace layerc::editor {

void AutomationEditor::update_view(const HWND dialog) {
  const auto selected =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_VIEW, CB_GETCURSEL, 0, 0);
  view_ = selected == CB_ERR ? 0 : std::clamp(static_cast<int>(selected), 0, 2);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VIEW, CB_SETCURSEL, view_, 0);

  constexpr int basic_ids[]{
      IDC_AUTOMATION_BASIC_LABEL,      IDC_AUTOMATION_PALETTE,
      IDC_AUTOMATION_ADD_NODE,         IDC_AUTOMATION_NODES,
      IDC_AUTOMATION_NAME_LABEL,       IDC_AUTOMATION_NAME,
      IDC_AUTOMATION_SELECTED_LABEL,   IDC_AUTOMATION_NODE_KIND,
      IDC_AUTOMATION_RULE_ID_LABEL,    IDC_AUTOMATION_RULE_ID,
      IDC_AUTOMATION_EVENT_LABEL,      IDC_AUTOMATION_EVENT,
      IDC_AUTOMATION_SOURCE_LABEL,     IDC_AUTOMATION_SOURCE,
      IDC_AUTOMATION_PRIORITY_LABEL,   IDC_AUTOMATION_PRIORITY,
      IDC_AUTOMATION_ACTION_LABEL,     IDC_AUTOMATION_ACTION_KIND,
      IDC_AUTOMATION_TARGET_LABEL,     IDC_AUTOMATION_TARGET,
      IDC_AUTOMATION_VALUE_LABEL,      IDC_AUTOMATION_VALUE,
      IDC_AUTOMATION_REPEATING,        IDC_AUTOMATION_APPLY_NODE,
      IDC_AUTOMATION_DELETE_NODE,      IDC_AUTOMATION_VALIDATION_LABEL,
      IDC_AUTOMATION_VALIDATION,       IDC_AUTOMATION_VALIDATE,
      IDC_AUTOMATION_RETRY};
  constexpr int advanced_ids[]{IDC_AUTOMATION_ADVANCED_LABEL,
                               IDC_AUTOMATION_JSON,
                               IDC_AUTOMATION_APPLY};
  constexpr int simulator_ids[]{
      IDC_AUTOMATION_SIM_LABEL,
      IDC_AUTOMATION_SIM_EVENT_LABEL,
      IDC_AUTOMATION_SIM_EVENT,
      IDC_AUTOMATION_SIM_SOURCE_LABEL,
      IDC_AUTOMATION_SIM_SOURCE,
      IDC_AUTOMATION_SIM_FIELDS_LABEL,
      IDC_AUTOMATION_SIM_FIELDS,
      IDC_AUTOMATION_SIM_ADD,
      IDC_AUTOMATION_SIM_RESET,
      IDC_AUTOMATION_SIM_STEP,
      IDC_AUTOMATION_SIM_PLAY,
      IDC_AUTOMATION_SIM_SPEED,
      IDC_AUTOMATION_TRACE,
      IDC_AUTOMATION_TRACE_LOAD,
      IDC_AUTOMATION_SAVE_TRACE,
      IDC_AUTOMATION_SIM_BREAKPOINT_LABEL,
      IDC_AUTOMATION_SIM_BREAKPOINT,
      IDC_AUTOMATION_SIM_ASSERTIONS_LABEL,
      IDC_AUTOMATION_SIM_ASSERTIONS,
      IDC_AUTOMATION_SIM_TRACE_LABEL,
      IDC_AUTOMATION_TRACE_TEXT};
  const auto show = [dialog](const auto &ids, const bool visible) {
    for (const int id : ids)
      ShowWindow(GetDlgItem(dialog, id), visible ? SW_SHOW : SW_HIDE);
  };
  show(basic_ids, view_ == 0);
  show(simulator_ids, view_ == 1);
  show(advanced_ids, view_ == 2);
  layout(dialog);
}

void AutomationEditor::layout(const HWND dialog) const {
  RECT client{};
  GetClientRect(dialog, &client);
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const int dpi = window_base_dpi(dialog);
  const auto scaled = [dpi](const int value) { return dpi_scaled(value, dpi); };
  const auto move = [dialog](const int id, const int x, const int y,
                             const int item_width, const int item_height) {
    move_control(dialog, id, x, y, item_width, item_height);
  };

  const int margin = scaled(12);
  const int gap = scaled(8);
  const int control_height = scaled(24);
  const int label_height = scaled(18);
  const int top = margin;
  const int extra = std::max(0, width - scaled(756));
  int left = margin;
  move(IDC_AUTOMATION_LIST_LABEL, left, top + scaled(3), scaled(60),
       label_height);
  left += scaled(60);
  move(IDC_AUTOMATION_LIST, left, top, scaled(140) + extra, control_height);
  left += scaled(140) + extra + gap;
  move(IDC_AUTOMATION_PRESET_LABEL, left, top + scaled(3), scaled(65),
       label_height);
  left += scaled(65);
  move(IDC_AUTOMATION_PRESETS, left, top, scaled(220), control_height);
  left += scaled(220) + gap;
  move(IDC_AUTOMATION_ADD_PRESET, left, top, scaled(54), control_height);
  left += scaled(60);
  move(IDC_AUTOMATION_REMOVE, left, top, scaled(58), control_height);
  left += scaled(66);
  move(IDC_AUTOMATION_ENABLED, left, top + scaled(2), scaled(70),
       label_height);
  const int view_top = top + control_height + gap;
  move(IDC_AUTOMATION_VIEW_LABEL, margin, view_top + scaled(3), scaled(40),
       label_height);
  move(IDC_AUTOMATION_VIEW, margin + scaled(40), view_top, scaled(150), control_height);

  const int close_top = height - margin - control_height;
  move(IDOK, width - margin - scaled(72), close_top, scaled(72),
       control_height);
  const int workspace_top = view_top + control_height + gap;
  const int workspace_bottom = close_top - gap;

  if (view_ == 0) {
    const int validation_height =
        scaled(validation_expanded_ ? 92 : 28);
    const int validation_top = workspace_bottom - validation_height;
    const int left_width = std::max(
        scaled(260), std::min(width * 42 / 100,
                              width - scaled(330) - margin * 2 - gap));
    const int right = margin + left_width + gap;
    const int right_width = width - margin - right;
    const int header_height = scaled(24);
    move(IDC_AUTOMATION_BASIC_LABEL, margin, workspace_top + scaled(3),
         scaled(105), label_height);
    move(IDC_AUTOMATION_PALETTE, margin + scaled(110), workspace_top,
         std::max(scaled(100), left_width - scaled(168)), control_height);
    move(IDC_AUTOMATION_ADD_NODE, margin + left_width - scaled(52),
         workspace_top, scaled(52), control_height);
    move(IDC_AUTOMATION_NODES, margin, workspace_top + header_height,
         left_width,
         std::max(scaled(90), validation_top - gap - workspace_top -
                                  header_height));

    const int field_label = scaled(92);
    const int field_left = right + field_label;
    const int field_width = std::max(1, right_width - field_label);
    const int row_height = scaled(27);
    const auto field = [&](const int label_id, const int control_id,
                           const int row) {
      const int y = workspace_top + row * row_height;
      move(label_id, right, y + scaled(3), field_label - scaled(5),
           label_height);
      move(control_id, field_left, y, field_width, control_height);
    };
    field(IDC_AUTOMATION_NAME_LABEL, IDC_AUTOMATION_NAME, 0);
    field(IDC_AUTOMATION_SELECTED_LABEL, IDC_AUTOMATION_NODE_KIND, 1);
    field(IDC_AUTOMATION_RULE_ID_LABEL, IDC_AUTOMATION_RULE_ID, 2);
    field(IDC_AUTOMATION_EVENT_LABEL, IDC_AUTOMATION_EVENT, 3);
    field(IDC_AUTOMATION_SOURCE_LABEL, IDC_AUTOMATION_SOURCE, 4);
    field(IDC_AUTOMATION_PRIORITY_LABEL, IDC_AUTOMATION_PRIORITY, 5);
    field(IDC_AUTOMATION_ACTION_LABEL, IDC_AUTOMATION_ACTION_KIND, 6);
    field(IDC_AUTOMATION_TARGET_LABEL, IDC_AUTOMATION_TARGET, 7);
    field(IDC_AUTOMATION_VALUE_LABEL, IDC_AUTOMATION_VALUE, 8);
    const int action_top = workspace_top + 9 * row_height;
    move(IDC_AUTOMATION_REPEATING, field_left, action_top + scaled(2),
         scaled(120), label_height);
    move(IDC_AUTOMATION_APPLY_NODE, field_left, action_top + row_height,
         scaled(82), control_height);
    move(IDC_AUTOMATION_DELETE_NODE, field_left + scaled(90),
         action_top + row_height, scaled(82), control_height);

    move(IDC_AUTOMATION_VALIDATION_LABEL, margin,
         validation_top + scaled(4), scaled(70), label_height);
    const int validate_width = scaled(68);
    const int retry_width = scaled(58);
    const int retry_left = width - margin - retry_width;
    const int validate_left = retry_left - gap - validate_width;
    move(IDC_AUTOMATION_VALIDATION, margin + scaled(74), validation_top,
         std::max(1, validate_left - gap - margin - scaled(74)),
         validation_height);
    move(IDC_AUTOMATION_VALIDATE, validate_left, validation_top,
         validate_width, control_height);
    move(IDC_AUTOMATION_RETRY, retry_left, validation_top, retry_width,
         control_height);
  } else if (view_ == 1) {
    move(IDC_AUTOMATION_SIM_LABEL, margin, workspace_top + scaled(3),
         scaled(90), label_height);
    const int first_row = workspace_top + scaled(24);
    const int source_left = margin + scaled(180);
    const int fields_left = source_left + scaled(180);
    move(IDC_AUTOMATION_SIM_EVENT_LABEL, margin, first_row, scaled(60),
         label_height);
    move(IDC_AUTOMATION_SIM_EVENT, margin, first_row + label_height,
         scaled(170), control_height);
    move(IDC_AUTOMATION_SIM_SOURCE_LABEL, source_left, first_row, scaled(60),
         label_height);
    move(IDC_AUTOMATION_SIM_SOURCE, source_left, first_row + label_height,
         scaled(170), control_height);
    move(IDC_AUTOMATION_SIM_FIELDS_LABEL, fields_left, first_row,
         scaled(60), label_height);
    move(IDC_AUTOMATION_SIM_FIELDS, fields_left, first_row + label_height,
         std::max(scaled(160), width - fields_left - margin - scaled(84)),
         control_height);
    move(IDC_AUTOMATION_SIM_ADD, width - margin - scaled(76),
         first_row + label_height, scaled(76), control_height);

    const int controls_top = first_row + label_height + control_height + gap;
    int x = margin;
    const auto button = [&](const int id, const int logical_width) {
      move(id, x, controls_top, scaled(logical_width), control_height);
      x += scaled(logical_width) + gap;
    };
    button(IDC_AUTOMATION_SIM_RESET, 58);
    button(IDC_AUTOMATION_SIM_STEP, 52);
    button(IDC_AUTOMATION_SIM_PLAY, 52);
    move(IDC_AUTOMATION_SIM_SPEED, x, controls_top, scaled(74),
         control_height);
    x += scaled(82);
    move(IDC_AUTOMATION_TRACE, x, controls_top + scaled(2), scaled(76),
         label_height);
    x += scaled(84);
    button(IDC_AUTOMATION_TRACE_LOAD, 90);
    button(IDC_AUTOMATION_SAVE_TRACE, 90);

    const int options_top = controls_top + control_height + gap;
    move(IDC_AUTOMATION_SIM_BREAKPOINT_LABEL, margin,
         options_top + scaled(3), scaled(100), label_height);
    move(IDC_AUTOMATION_SIM_BREAKPOINT, margin + scaled(104), options_top,
         scaled(170), control_height);
    const int assertions_left = margin + scaled(286);
    move(IDC_AUTOMATION_SIM_ASSERTIONS_LABEL, assertions_left,
         options_top + scaled(3), scaled(145), label_height);
    move(IDC_AUTOMATION_SIM_ASSERTIONS, assertions_left + scaled(150),
         options_top,
         std::max(scaled(160), width - assertions_left - scaled(150) - margin),
         control_height);
    const int trace_top = options_top + control_height + gap;
    move(IDC_AUTOMATION_SIM_TRACE_LABEL, margin, trace_top + scaled(3),
         scaled(55), label_height);
    move(IDC_AUTOMATION_TRACE_TEXT, margin + scaled(58), trace_top,
         width - margin * 2 - scaled(58),
         std::max(scaled(100), workspace_bottom - trace_top));
  } else {
    move(IDC_AUTOMATION_ADVANCED_LABEL, margin, workspace_top + scaled(3),
         width - margin * 2, label_height);
    const int apply_top = workspace_bottom - control_height;
    move(IDC_AUTOMATION_JSON, margin, workspace_top + scaled(24),
         width - margin * 2,
         std::max(scaled(100), apply_top - gap - workspace_top - scaled(24)));
    move(IDC_AUTOMATION_APPLY, width - margin - scaled(92), apply_top,
         scaled(92), control_height);
  }
}

} // namespace layerc::editor
