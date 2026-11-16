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
  HWND dialog{};
  DialogLayout ui;
  int width{};
  int margin{};
  int gap{};
  int control_height{};
  int label_height{};
  int workspace_top{};
  int workspace_bottom{};
  // The narrowest client width the header rows fit, and the client height
  // below the workspace (the Close row and margins).
  int min_width{};
  int footer_height{};
};

// What a view needs to show every control without overlap: its narrowest
// client width and the lowest workspace bottom its content fits above. Each
// view computes it with the same arithmetic that places its controls, so the
// dialog's minimum size cannot drift from its layout.
struct ViewExtent {
  int width{};
  int workspace_bottom{};
};

// Width of a label's current text in its own font, plus a small gap, so
// labels size to their words instead of clipping ("Automatio").
int text_width(const HWND dialog, const int id, const int minimum) {
  const HWND control = GetDlgItem(dialog, id);
  if (control == nullptr)
    return minimum;
  wchar_t text[128]{};
  const int length = GetWindowTextW(control, text, 128);
  const HDC dc = GetDC(dialog);
  if (dc == nullptr)
    return minimum;
  const HGDIOBJ previous = SelectObject(
      dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
  // DrawText drops the access-key '&' the way the label renders it.
  RECT extent{};
  DrawTextW(dc, text, length, &extent, DT_CALCRECT | DT_SINGLELINE);
  SelectObject(dc, previous);
  ReleaseDC(dialog, dc);
  return std::max(minimum, static_cast<int>(extent.right) +
                               MulDiv(12, static_cast<int>(GetDpiForWindow(dialog)), 96));
}

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
    IDC_AUTOMATION_SIM_SPEED_LABEL,
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

ViewExtent layout_basic_view(const AutomationFrame &f,
                             const bool validation_expanded) {
  const DialogLayout &ui = f.ui;
  const int header_height = f.control_height + ui.scaled(8);
  const int basic_label =
      text_width(f.dialog, IDC_AUTOMATION_BASIC_LABEL, ui.scaled(60));
  const int add_width = ui.scaled(64);
  const int palette_minimum = ui.scaled(100);
  // The left column is never narrower than its palette row.
  const int left_minimum =
      std::max(ui.scaled(260),
               basic_label + palette_minimum + ui.scaled(8) + add_width);

  int field_label = ui.scaled(72);
  for (const int id :
       {IDC_AUTOMATION_NAME_LABEL, IDC_AUTOMATION_SELECTED_LABEL,
        IDC_AUTOMATION_RULE_ID_LABEL, IDC_AUTOMATION_EVENT_LABEL,
        IDC_AUTOMATION_SOURCE_LABEL, IDC_AUTOMATION_PRIORITY_LABEL,
        IDC_AUTOMATION_ACTION_LABEL, IDC_AUTOMATION_TARGET_LABEL,
        IDC_AUTOMATION_VALUE_LABEL})
    field_label = std::max(field_label, text_width(f.dialog, id, 0));
  // The selected item's two action buttons set the narrowest field column.
  const int item_button = ui.scaled(104);
  const int item_buttons = item_button * 2 + ui.scaled(8);
  const int right_minimum =
      std::max(ui.scaled(330),
               field_label + std::max(item_buttons, ui.scaled(160)));
  const int left_width = std::max(
      left_minimum, std::min(f.width * 42 / 100,
                             f.width - right_minimum - f.margin * 2 - f.gap));
  const int right = f.margin + left_width + f.gap;
  const int right_width = f.width - f.margin - right;
  const int field_left = right + field_label;
  const int row_height = f.control_height + ui.scaled(8);
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
                 f.control_height);
    y += row_height;
  }
  ui.move(IDC_AUTOMATION_REPEATING, field_left, y,
          ui.scaled(160), f.control_height);
  ui.move(IDC_AUTOMATION_APPLY_NODE, field_left, y + row_height,
          item_button, f.control_height);
  ui.move(IDC_AUTOMATION_DELETE_NODE, field_left + item_button + ui.scaled(8),
          y + row_height, item_button, f.control_height);
  const int form_bottom = y + row_height + f.control_height;

  // Validation stays at the bottom, at least four rows so several findings
  // read without scrolling (taller while expanded and the room allows), and
  // never over the form above: the extent below reserves its collapsed
  // height, and only a window smaller than that extent shrinks it further.
  const int validation_collapsed = ui.scaled(104);
  const int validation_height = std::clamp(
      f.workspace_bottom - form_bottom - f.gap, f.control_height,
      validation_expanded ? ui.scaled(200) : validation_collapsed);
  const int validation_top = f.workspace_bottom - validation_height;
  const int list_minimum = ui.scaled(90);

  ui.move(IDC_AUTOMATION_BASIC_LABEL, f.margin,
          f.workspace_top + (f.control_height - f.label_height) / 2,
          basic_label, f.label_height);
  ui.move(IDC_AUTOMATION_PALETTE, f.margin + basic_label, f.workspace_top,
          std::max(palette_minimum,
                   left_width - basic_label - add_width - ui.scaled(8)),
          ui.scaled(240));
  ui.move(IDC_AUTOMATION_ADD_NODE, f.margin + left_width - add_width,
          f.workspace_top, add_width, f.control_height);
  ui.move(IDC_AUTOMATION_NODES, f.margin, f.workspace_top + header_height,
          left_width,
          std::max(list_minimum, validation_top - f.gap - f.workspace_top -
                                     header_height));

  const int validation_label =
      text_width(f.dialog, IDC_AUTOMATION_VALIDATION_LABEL, ui.scaled(60));
  ui.move(IDC_AUTOMATION_VALIDATION_LABEL, f.margin,
          validation_top + ui.scaled(4), validation_label, f.label_height);
  const int validate_width = ui.scaled(88);
  const int retry_width = ui.scaled(72);
  const int retry_left = f.width - f.margin - retry_width;
  const int validate_left = retry_left - f.gap - validate_width;
  ui.move(IDC_AUTOMATION_VALIDATION, f.margin + validation_label,
          validation_top,
          std::max(1, validate_left - f.gap - f.margin - validation_label),
          validation_height);
  ui.move(IDC_AUTOMATION_VALIDATE, validate_left, validation_top,
          validate_width, f.control_height);
  ui.move(IDC_AUTOMATION_RETRY, retry_left, validation_top, retry_width,
          f.control_height);

  const int content_bottom =
      std::max(form_bottom, f.workspace_top + header_height + list_minimum);
  return {.width = std::max(f.margin * 2 + left_minimum + f.gap + right_minimum,
                            f.margin * 2 + validation_label + ui.scaled(160) +
                                f.gap * 2 + validate_width + retry_width),
          .workspace_bottom = content_bottom + f.gap + validation_collapsed};
}

ViewExtent layout_simulator_view(const AutomationFrame &f,
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
  const int fields_minimum = ui.scaled(160);
  ui.move(IDC_AUTOMATION_SIM_FIELDS, fields_left, input_top,
          std::max(fields_minimum,
                   f.width - fields_left - f.margin - ui.scaled(84)),
          f.control_height);
  ui.move(IDC_AUTOMATION_SIM_ADD, f.width - f.margin - ui.scaled(76),
          input_top, ui.scaled(76), f.control_height);
  const int input_row_width =
      fields_left + fields_minimum + ui.scaled(84) + f.margin;

  const int controls_top = input_top + f.control_height + f.gap;
  int x = f.margin;
  ui.next_button(IDC_AUTOMATION_SIM_RESET, x, controls_top, 58,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SIM_STEP, x, controls_top, 52,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SIM_PLAY, x, controls_top, 52,
                 f.control_height, f.gap);
  // The visible label also names the speed list for screen readers (the
  // static before it in tab order).
  const int speed_label =
      text_width(f.dialog, IDC_AUTOMATION_SIM_SPEED_LABEL, 0);
  ui.row_label(IDC_AUTOMATION_SIM_SPEED_LABEL, x, controls_top, speed_label,
               f.control_height);
  x += speed_label;
  ui.move(IDC_AUTOMATION_SIM_SPEED, x, controls_top, ui.scaled(74),
          f.control_height);
  x += ui.scaled(82);
  // The checkbox sizes to its label (box, gap, text) instead of clipping it
  // to "Live tr...".
  const int trace_width =
      text_width(f.dialog, IDC_AUTOMATION_TRACE, 0) + ui.scaled(18 + 8);
  ui.move(IDC_AUTOMATION_TRACE, x, controls_top + ui.scaled(2), trace_width,
          f.label_height);
  x += trace_width + ui.scaled(8);
  ui.next_button(IDC_AUTOMATION_TRACE_LOAD, x, controls_top, 90,
                 f.control_height, f.gap);
  ui.next_button(IDC_AUTOMATION_SAVE_TRACE, x, controls_top, 90,
                 f.control_height, f.gap);
  const int controls_row_width = x - f.gap + f.margin;

  const int options_top = controls_top + f.control_height + f.gap;
  const int breakpoint_label =
      text_width(f.dialog, IDC_AUTOMATION_SIM_BREAKPOINT_LABEL, 0);
  ui.move(IDC_AUTOMATION_SIM_BREAKPOINT_LABEL, f.margin,
          options_top + ui.scaled(5), breakpoint_label, f.label_height);
  ui.move(IDC_AUTOMATION_SIM_BREAKPOINT, f.margin + breakpoint_label,
          options_top, ui.scaled(170), f.control_height);
  const int assertions_left =
      f.margin + breakpoint_label + ui.scaled(170) + f.gap * 2;
  const int assertions_label =
      text_width(f.dialog, IDC_AUTOMATION_SIM_ASSERTIONS_LABEL, 0);
  ui.move(IDC_AUTOMATION_SIM_ASSERTIONS_LABEL, assertions_left,
          options_top + ui.scaled(5), assertions_label, f.label_height);
  const int assertions_minimum = ui.scaled(160);
  ui.move(IDC_AUTOMATION_SIM_ASSERTIONS, assertions_left + assertions_label,
          options_top,
          std::max(assertions_minimum,
                   f.width - assertions_left - assertions_label - f.margin),
          f.control_height);
  const int options_row_width =
      assertions_left + assertions_label + assertions_minimum + f.margin;
  const int trace_top = options_top + f.control_height + f.gap;
  const int trace_minimum = ui.scaled(100);
  ui.move(IDC_AUTOMATION_SIM_TRACE_LABEL, f.margin, trace_top + ui.scaled(3),
          ui.scaled(55), f.label_height);
  ui.move(IDC_AUTOMATION_TRACE_TEXT, f.margin + ui.scaled(58), trace_top,
          f.width - f.margin * 2 - ui.scaled(58),
          std::max(trace_minimum, f.workspace_bottom - trace_top));
  return {.width = std::max({input_row_width, controls_row_width,
                             options_row_width}),
          .workspace_bottom = trace_top + trace_minimum};
}

ViewExtent layout_advanced_view(const AutomationFrame &f,
                                const bool /*validation_expanded*/) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_AUTOMATION_ADVANCED_LABEL, f.margin,
          f.workspace_top + ui.scaled(3), f.width - f.margin * 2,
          f.label_height);
  const int json_top = f.workspace_top + ui.scaled(24);
  const int json_minimum = ui.scaled(100);
  const int apply_top = f.workspace_bottom - f.control_height;
  ui.move(IDC_AUTOMATION_JSON, f.margin, json_top, f.width - f.margin * 2,
          std::max(json_minimum, apply_top - f.gap - json_top));
  ui.move(IDC_AUTOMATION_APPLY, f.width - f.margin - ui.scaled(92), apply_top,
          ui.scaled(92), f.control_height);
  return {.width = f.margin * 2 +
                   text_width(f.dialog, IDC_AUTOMATION_ADVANCED_LABEL, 0),
          .workspace_bottom =
              json_top + json_minimum + f.gap + f.control_height};
}

// One entry per IDC_AUTOMATION_VIEW item, in combo order: the controls the
// view shows, how it lays them out, and the extent that layout needs.
struct AutomationView {
  std::span<const int> ids;
  ViewExtent (*layout)(const AutomationFrame &frame, bool validation_expanded);
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
  const int control_height = ui.scaled(native_control_height);
  const int label_height = ui.scaled(20);
  const int top = margin;
  // Right-anchored preset/action cluster; the automation list takes the rest.
  const int list_label = text_width(dialog, IDC_AUTOMATION_LIST_LABEL, 0);
  const int preset_label = text_width(dialog, IDC_AUTOMATION_PRESET_LABEL, 0);
  const int enabled_width =
      text_width(dialog, IDC_AUTOMATION_ENABLED, 0) + ui.scaled(24);
  const int new_width = ui.scaled(60);
  const int remove_width = ui.scaled(68);
  const int presets_width = ui.scaled(200);
  int right = width - margin - enabled_width;
  ui.move(IDC_AUTOMATION_ENABLED, right, top, enabled_width, control_height);
  right -= gap + remove_width;
  ui.move(IDC_AUTOMATION_REMOVE, right, top, remove_width, control_height);
  right -= gap + new_width;
  ui.move(IDC_AUTOMATION_ADD_PRESET, right, top, new_width, control_height);
  right -= gap + presets_width;
  ui.move(IDC_AUTOMATION_PRESETS, right, top, presets_width, ui.scaled(200));
  right -= preset_label;
  ui.row_label(IDC_AUTOMATION_PRESET_LABEL, right, top, preset_label,
               control_height);
  ui.row_label(IDC_AUTOMATION_LIST_LABEL, margin, top, list_label,
               control_height);
  ui.move(IDC_AUTOMATION_LIST, margin + list_label, top,
          std::max(ui.scaled(100), right - gap * 2 - margin - list_label),
          ui.scaled(240));
  const int view_top = top + control_height + gap;
  const int view_label = text_width(dialog, IDC_AUTOMATION_VIEW_LABEL, 0);
  ui.row_label(IDC_AUTOMATION_VIEW_LABEL, margin, view_top, view_label,
               control_height);
  ui.move(IDC_AUTOMATION_VIEW, margin + view_label, view_top, ui.scaled(170),
          ui.scaled(160));

  const int close_top = height - margin - control_height;
  ui.move(IDOK, width - margin - ui.scaled(88), close_top, ui.scaled(88),
          control_height);
  // The list keeps its 100 px minimum beside the right-anchored cluster.
  const int header_width = margin * 2 + list_label + ui.scaled(100) +
                           gap * 5 + preset_label + presets_width + new_width +
                           remove_width + enabled_width;
  return {.dialog = dialog,
          .ui = ui,
          .width = width,
          .margin = margin,
          .gap = gap,
          .control_height = control_height,
          .label_height = label_height,
          .workspace_top = view_top + control_height + gap,
          .workspace_bottom = close_top - gap,
          .min_width = std::max(header_width,
                                margin * 2 + view_label + ui.scaled(170)),
          .footer_height = gap + control_height + margin};
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

// Every view is laid out (the inactive ones while hidden), so the dialog's
// minimum is the largest view extent and switching views never needs a
// bigger window. A window smaller than that minimum (a DPI or font change)
// grows to it, kept on its work area.
void AutomationEditor::layout(const HWND dialog) {
  const AutomationFrame frame = layout_automation_header(dialog);
  SIZE minimum{frame.min_width, 0};
  for (const auto &view : automation_views) {
    const ViewExtent extent = view.layout(frame, validation_expanded_);
    minimum.cx = std::max(minimum.cx, static_cast<LONG>(extent.width));
    minimum.cy = std::max(
        minimum.cy,
        static_cast<LONG>(extent.workspace_bottom + frame.footer_height));
  }
  minimum_client_ = minimum;
  grow_client_to(dialog, minimum);
}

} // namespace layerc::editor
