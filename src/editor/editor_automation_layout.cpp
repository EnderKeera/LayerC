#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include <windows.h>

namespace layerc::editor {
namespace {

// Workspace below the automation list and view rows, shared by every view.
struct AutomationFrame {
  DialogLayout ui;
  int width{};
  int margin{};
  int gap{};
  int control_height{};
  int label_height{};
  int workspace_top{};
  int workspace_bottom{};
};

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
constexpr int advanced_ids[]{IDC_AUTOMATION_ADVANCED_LABEL,
                             IDC_AUTOMATION_JSON, IDC_AUTOMATION_APPLY};

void layout_basic_view(const AutomationFrame &f,
                       const bool validation_expanded) {
  const DialogLayout &ui = f.ui;
  const int validation_height = ui.scaled(validation_expanded ? 92 : 28);
  const int validation_top = f.workspace_bottom - validation_height;
  const int left_width = std::max(
      ui.scaled(260), std::min(f.width * 42 / 100,
                               f.width - ui.scaled(330) - f.margin * 2 - f.gap));
  const int right = f.margin + left_width + f.gap;
  const int right_width = f.width - f.margin - right;
  const int header_height = ui.scaled(24);
  ui.move(IDC_AUTOMATION_BASIC_LABEL, f.margin, f.workspace_top + ui.scaled(3),
          ui.scaled(105), f.label_height);
  ui.move(IDC_AUTOMATION_PALETTE, f.margin + ui.scaled(110), f.workspace_top,
          std::max(ui.scaled(100), left_width - ui.scaled(168)),
          f.control_height);
  ui.move(IDC_AUTOMATION_ADD_NODE, f.margin + left_width - ui.scaled(52),
          f.workspace_top, ui.scaled(52), f.control_height);
  ui.move(IDC_AUTOMATION_NODES, f.margin, f.workspace_top + header_height,
          left_width,
          std::max(ui.scaled(90), validation_top - f.gap - f.workspace_top -
                                      header_height));

  const int field_label = ui.scaled(92);
  const int field_left = right + field_label;
  const int row_height = ui.scaled(27);
  constexpr std::array<std::array<int, 2>, 9> fields{{
      {IDC_AUTOMATION_NAME_LABEL, IDC_AUTOMATION_NAME},
      {IDC_AUTOMATION_SELECTED_LABEL, IDC_AUTOMATION_NODE_KIND},
      {IDC_AUTOMATION_RULE_ID_LABEL, IDC_AUTOMATION_RULE_ID},
      {IDC_AUTOMATION_EVENT_LABEL, IDC_AUTOMATION_EVENT},
      {IDC_AUTOMATION_SOURCE_LABEL, IDC_AUTOMATION_SOURCE},
      {IDC_AUTOMATION_PRIORITY_LABEL, IDC_AUTOMATION_PRIORITY},
      {IDC_AUTOMATION_ACTION_LABEL, IDC_AUTOMATION_ACTION_KIND},
      {IDC_AUTOMATION_TARGET_LABEL, IDC_AUTOMATION_TARGET},
      {IDC_AUTOMATION_VALUE_LABEL, IDC_AUTOMATION_VALUE},
  }};
  int y = f.workspace_top;
  for (const auto &[label_id, control_id] : fields) {
    ui.field_row(label_id, control_id, right, y, field_label, right_width,
                 f.label_height, f.control_height);
    y += row_height;
  }
  ui.move(IDC_AUTOMATION_REPEATING, field_left, y + ui.scaled(2),
          ui.scaled(120), f.label_height);
  ui.move(IDC_AUTOMATION_APPLY_NODE, field_left, y + row_height,
          ui.scaled(82), f.control_height);
  ui.move(IDC_AUTOMATION_DELETE_NODE, field_left + ui.scaled(90),
          y + row_height, ui.scaled(82), f.control_height);

  ui.move(IDC_AUTOMATION_VALIDATION_LABEL, f.margin,
          validation_top + ui.scaled(4), ui.scaled(70), f.label_height);
  const int validate_width = ui.scaled(68);
  const int retry_width = ui.scaled(58);
  const int retry_left = f.width - f.margin - retry_width;
  const int validate_left = retry_left - f.gap - validate_width;
  ui.move(IDC_AUTOMATION_VALIDATION, f.margin + ui.scaled(74), validation_top,
          std::max(1, validate_left - f.gap - f.margin - ui.scaled(74)),
          validation_height);
  ui.move(IDC_AUTOMATION_VALIDATE, validate_left, validation_top,
          validate_width, f.control_height);
  ui.move(IDC_AUTOMATION_RETRY, retry_left, validation_top, retry_width,
          f.control_height);
}

void layout_simulator_view(const AutomationFrame &f,
                           const bool /*validation_expanded*/) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_AUTOMATION_SIM_LABEL, f.margin, f.workspace_top + ui.scaled(3),
          ui.scaled(90), f.label_height);
  const int first_row = f.workspace_top + ui.scaled(24);
  const int input_top = first_row + f.label_height;
  const int source_left = f.margin + ui.scaled(180);
  const int fields_left = source_left + ui.scaled(180);
  ui.move(IDC_AUTOMATION_SIM_EVENT_LABEL, f.margin, first_row, ui.scaled(60),
          f.label_height);
  ui.move(IDC_AUTOMATION_SIM_EVENT, f.margin, input_top, ui.scaled(170),
          f.control_height);
  ui.move(IDC_AUTOMATION_SIM_SOURCE_LABEL, source_left, first_row,
          ui.scaled(60), f.label_height);
  ui.move(IDC_AUTOMATION_SIM_SOURCE, source_left, input_top, ui.scaled(170),
          f.control_height);
  ui.move(IDC_AUTOMATION_SIM_FIELDS_LABEL, fields_left, first_row,
          ui.scaled(60), f.label_height);
  ui.move(IDC_AUTOMATION_SIM_FIELDS, fields_left, input_top,
          std::max(ui.scaled(160),
                   f.width - fields_left - f.margin - ui.scaled(84)),
          f.control_height);
  ui.move(IDC_AUTOMATION_SIM_ADD, f.width - f.margin - ui.scaled(76),
          input_top, ui.scaled(76), f.control_height);

  const int controls_top = input_top + f.control_height + f.gap;
  int x = f.margin;
  ui.next_button(IDC_AUTOMATION_SIM_RESET, x, controls_top, 58,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SIM_STEP, x, controls_top, 52,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SIM_PLAY, x, controls_top, 52,
                 f.control_height, f.gap);
  ui.move(IDC_AUTOMATION_SIM_SPEED, x, controls_top, ui.scaled(74),
          f.control_height);
  x += ui.scaled(82);
  ui.move(IDC_AUTOMATION_TRACE, x, controls_top + ui.scaled(2), ui.scaled(76),
          f.label_height);
  x += ui.scaled(84);
  ui.next_button(IDC_AUTOMATION_TRACE_LOAD, x, controls_top, 90,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SAVE_TRACE, x, controls_top, 90,
                 f.control_height, f.gap);

  const int options_top = controls_top + f.control_height + f.gap;
  ui.move(IDC_AUTOMATION_SIM_BREAKPOINT_LABEL, f.margin,
          options_top + ui.scaled(3), ui.scaled(100), f.label_height);
  ui.move(IDC_AUTOMATION_SIM_BREAKPOINT, f.margin + ui.scaled(104),
          options_top, ui.scaled(170), f.control_height);
  const int assertions_left = f.margin + ui.scaled(286);
  ui.move(IDC_AUTOMATION_SIM_ASSERTIONS_LABEL, assertions_left,
          options_top + ui.scaled(3), ui.scaled(145), f.label_height);
  ui.move(IDC_AUTOMATION_SIM_ASSERTIONS, assertions_left + ui.scaled(150),
          options_top,
          std::max(ui.scaled(160),
                   f.width - assertions_left - ui.scaled(150) - f.margin),
          f.control_height);
  const int trace_top = options_top + f.control_height + f.gap;
  ui.move(IDC_AUTOMATION_SIM_TRACE_LABEL, f.margin, trace_top + ui.scaled(3),
          ui.scaled(55), f.label_height);
  ui.move(IDC_AUTOMATION_TRACE_TEXT, f.margin + ui.scaled(58), trace_top,
          f.width - f.margin * 2 - ui.scaled(58),
          std::max(ui.scaled(100), f.workspace_bottom - trace_top));
}

void layout_advanced_view(const AutomationFrame &f,
                          const bool /*validation_expanded*/) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_AUTOMATION_ADVANCED_LABEL, f.margin,
          f.workspace_top + ui.scaled(3), f.width - f.margin * 2,
          f.label_height);
  const int apply_top = f.workspace_bottom - f.control_height;
  ui.move(IDC_AUTOMATION_JSON, f.margin, f.workspace_top + ui.scaled(24),
          f.width - f.margin * 2,
          std::max(ui.scaled(100),
                   apply_top - f.gap - f.workspace_top - ui.scaled(24)));
  ui.move(IDC_AUTOMATION_APPLY, f.width - f.margin - ui.scaled(92), apply_top,
          ui.scaled(92), f.control_height);
}

// One entry per IDC_AUTOMATION_VIEW item, in combo order: the controls the
// view shows and how it lays them out.
struct AutomationView {
  std::span<const int> ids;
  void (*layout)(const AutomationFrame &frame, bool validation_expanded);
};

constexpr std::array automation_views{
    AutomationView{basic_ids, &layout_basic_view},
    AutomationView{simulator_ids, &layout_simulator_view},
    AutomationView{advanced_ids, &layout_advanced_view},
};

// Places the list, preset, and view rows plus the close button, and returns
// the workspace frame left for the active view.
AutomationFrame layout_automation_header(const HWND dialog) {
  RECT client{};
  GetClientRect(dialog, &client);
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  const DialogLayout ui(dialog, window_base_dpi(dialog));

  const int margin = ui.scaled(12);
  const int gap = ui.scaled(8);
  const int control_height = ui.scaled(24);
  const int label_height = ui.scaled(18);
  const int top = margin;
  const int extra = std::max(0, width - ui.scaled(756));
  int left = margin;
  ui.move(IDC_AUTOMATION_LIST_LABEL, left, top + ui.scaled(3), ui.scaled(60),
          label_height);
  left += ui.scaled(60);
  ui.move(IDC_AUTOMATION_LIST, left, top, ui.scaled(140) + extra,
          control_height);
  left += ui.scaled(140) + extra + gap;
  ui.move(IDC_AUTOMATION_PRESET_LABEL, left, top + ui.scaled(3),
          ui.scaled(65), label_height);
  left += ui.scaled(65);
  ui.move(IDC_AUTOMATION_PRESETS, left, top, ui.scaled(220), control_height);
  left += ui.scaled(220) + gap;
  ui.move(IDC_AUTOMATION_ADD_PRESET, left, top, ui.scaled(54),
          control_height);
  left += ui.scaled(60);
  ui.move(IDC_AUTOMATION_REMOVE, left, top, ui.scaled(58), control_height);
  left += ui.scaled(66);
  ui.move(IDC_AUTOMATION_ENABLED, left, top + ui.scaled(2), ui.scaled(70),
          label_height);
  const int view_top = top + control_height + gap;
  ui.move(IDC_AUTOMATION_VIEW_LABEL, margin, view_top + ui.scaled(3),
          ui.scaled(40), label_height);
  ui.move(IDC_AUTOMATION_VIEW, margin + ui.scaled(40), view_top,
          ui.scaled(150), control_height);

  const int close_top = height - margin - control_height;
  ui.move(IDOK, width - margin - ui.scaled(72), close_top, ui.scaled(72),
          control_height);
  return {.ui = ui,
          .width = width,
          .margin = margin,
          .gap = gap,
          .control_height = control_height,
          .label_height = label_height,
          .workspace_top = view_top + control_height + gap,
          .workspace_bottom = close_top - gap};
}

[[nodiscard]] std::size_t view_slot(const int view) noexcept {
  return static_cast<std::size_t>(
      std::clamp(view, 0, static_cast<int>(automation_views.size()) - 1));
}

} // namespace

void AutomationEditor::update_view(const HWND dialog) {
  const auto selected =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_VIEW, CB_GETCURSEL, 0, 0);
  view_ = selected == CB_ERR ? 0 : std::clamp(static_cast<int>(selected), 0, 2);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VIEW, CB_SETCURSEL, view_, 0);

  const std::size_t active = view_slot(view_);
  for (std::size_t slot = 0; slot < automation_views.size(); ++slot) {
    for (const int id : automation_views[slot].ids)
      ShowWindow(GetDlgItem(dialog, id), slot == active ? SW_SHOW : SW_HIDE);
  }
  layout(dialog);
}

void AutomationEditor::layout(const HWND dialog) const {
  const AutomationFrame frame = layout_automation_header(dialog);
  automation_views[view_slot(view_)].layout(frame, validation_expanded_);
}

} // namespace layerc::editor
