#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
namespace {

// Controls that exist only in advanced mode. MainLayout::advanced is the one
// advanced_visible() read that every section below consults.
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

// Main window columns (sidebar | content | visual and preview) above the
// status row, in physical pixels at the geometry DPI.
struct MainLayout {
  DialogLayout ui;
  bool advanced{};
  int width{};
  int margin{};
  int gap{};
  int section_top{};
  int heading_height{};
  int row_height{};
  int status_top{};
  int sidebar_width{};
  int content_left{};
  int content_right{};
  int content_width{};
  int right_left{};
  int right_width{};
};

MainLayout make_main_layout(const HWND window, const bool advanced) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  const DialogLayout ui(window, geometry_dpi(window, 1080, 700));
  const int margin = ui.scaled(12);
  const int sidebar_width =
      std::clamp(width / 5, ui.scaled(180), ui.scaled(270));
  const int content_left = margin + sidebar_width + ui.scaled(18);
  const int right_width =
      std::clamp(width * 30 / 100, ui.scaled(285), ui.scaled(420));
  const int right_left = width - margin - right_width;
  const int content_right = right_left - ui.scaled(18);
  return {.ui = ui,
          .advanced = advanced,
          .width = width,
          .margin = margin,
          .gap = ui.scaled(14),
          .section_top = ui.scaled(12),
          .heading_height = ui.scaled(18),
          .row_height = ui.scaled(24),
          .status_top = height - ui.scaled(27),
          .sidebar_width = sidebar_width,
          .content_left = content_left,
          .content_right = content_right,
          .content_width = std::max(1, content_right - content_left),
          .right_left = right_left,
          .right_width = right_width};
}

void show_advanced_controls(const HWND window, const bool advanced) {
  for (const int id : advanced_controls) {
    if (const HWND control = GetDlgItem(window, id); control != nullptr)
      ShowWindow(control, advanced ? SW_SHOWNA : SW_HIDE);
  }
}

void layout_sidebar(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_SAVED_PROFILES_LABEL, f.margin, f.section_top, f.sidebar_width,
          f.heading_height);
  const int list_top = f.section_top + f.heading_height + ui.scaled(4);
  ui.move(IDC_PROFILE_LIST, f.margin, list_top, f.sidebar_width,
          std::max(ui.scaled(120), f.status_top - list_top - ui.scaled(8)));
}

// Returns the top of the first row below the profile fields.
int layout_profile_fields(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_PROFILE_SECTION_LABEL, f.content_left, f.section_top,
          f.content_width, f.heading_height);
  ui.move(IDC_ADVANCED, f.content_right - ui.scaled(104),
          f.section_top - ui.scaled(2), ui.scaled(104), ui.scaled(24));
  const int label_width = ui.scaled(86);
  const int field_left = f.content_left + label_width + ui.scaled(8);
  const int name_top = f.section_top + f.heading_height + ui.scaled(5);
  ui.move(IDC_PROFILE_NAME_LABEL, f.content_left, name_top + ui.scaled(3),
          label_width, f.heading_height);
  ui.move(IDC_PROFILE_NAME, field_left, name_top, f.content_right - field_left,
          f.row_height);

  int row_top = name_top + ui.scaled(29);
  if (f.advanced) {
    ui.move(IDC_PROFILE_ID_LABEL, f.content_left, row_top + ui.scaled(3),
            label_width, f.heading_height);
    ui.move(IDC_PROFILE_ID, field_left, row_top, f.content_right - field_left,
            f.row_height);
    row_top += ui.scaled(29);
  }
  const int choose_width = ui.scaled(164);
  ui.move(IDC_APPLICATION_LABEL, f.content_left, row_top + ui.scaled(3),
          label_width, f.heading_height);
  ui.move(IDC_APPLICATION_PATH, field_left, row_top,
          f.content_right - choose_width - ui.scaled(7) - field_left,
          f.row_height);
  ui.move(IDC_CHOOSE_APPLICATION, f.content_right - choose_width, row_top,
          choose_width, f.row_height);
  row_top += ui.scaled(29);
  if (f.advanced) {
    ui.move(IDC_APPLICATION_EXACT_PATH, field_left, row_top,
            f.content_right - field_left, ui.scaled(24));
    row_top += ui.scaled(29);
  }
  return row_top;
}

// Cooldown list, its buttons, and the name/duration row; returns the top of
// the binding row.
int layout_cooldown_list(const MainLayout &f, const int profile_bottom) {
  const DialogLayout &ui = f.ui;
  const int heading_top = profile_bottom + ui.scaled(9);
  const int list_top = heading_top + f.heading_height + ui.scaled(4);
  const int button_width = ui.scaled(64);
  const int list_width = f.content_width - button_width - ui.scaled(8);
  const int list_height = ui.scaled(f.advanced ? 78 : 90);
  ui.move(IDC_COOLDOWNS_LABEL, f.content_left, heading_top, f.content_width,
          f.heading_height);
  ui.move(IDC_COOLDOWN_LIST, f.content_left, list_top, list_width,
          list_height);
  const int button_left = f.content_left + list_width + ui.scaled(8);
  ui.move(IDC_ADD_COOLDOWN, button_left, list_top, button_width,
          ui.scaled(27));
  ui.move(IDC_REMOVE_COOLDOWN, button_left, list_top + ui.scaled(33),
          button_width, ui.scaled(27));

  const int name_top = list_top + list_height + ui.scaled(7);
  const int name_label_width = ui.scaled(48);
  const int duration_label_width = ui.scaled(112);
  const int duration_field_width = ui.scaled(72);
  const int duration_left = f.content_right - duration_field_width;
  ui.move(IDC_COOLDOWN_NAME_LABEL, f.content_left, name_top + ui.scaled(3),
          name_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_NAME, f.content_left + name_label_width, name_top,
          duration_left - duration_label_width - ui.scaled(8) -
              (f.content_left + name_label_width),
          f.row_height);
  ui.move(IDC_COOLDOWN_DURATION_LABEL, duration_left - duration_label_width,
          name_top + ui.scaled(3), duration_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_DURATION, duration_left, name_top,
          duration_field_width, f.row_height);
  return name_top + ui.scaled(31);
}

// Advanced cooldown id/charges, policy, and group rows; returns the top of
// the bars heading.
int layout_cooldown_technical(const MainLayout &f, const int technical_top,
                              const int groups_label_width) {
  const DialogLayout &ui = f.ui;
  const int technical_label_width = ui.scaled(78);
  const int charges_label_width = ui.scaled(60);
  const int charges_width = ui.scaled(54);
  ui.move(IDC_COOLDOWN_ID_LABEL, f.content_left, technical_top + ui.scaled(3),
          technical_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_ID, f.content_left + technical_label_width,
          technical_top,
          f.content_width - technical_label_width - charges_label_width -
              charges_width - ui.scaled(8),
          f.row_height);
  ui.move(IDC_COOLDOWN_CHARGES_LABEL,
          f.content_right - charges_label_width - charges_width,
          technical_top + ui.scaled(3), charges_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_CHARGES, f.content_right - charges_width,
          technical_top, charges_width, f.row_height);

  const int policy_top = technical_top + ui.scaled(31);
  const int half = (f.content_width - ui.scaled(10)) / 2;
  const int policy_label_width = ui.scaled(62);
  ui.move(IDC_COOLDOWN_RETRIGGER_LABEL, f.content_left,
          policy_top + ui.scaled(3), policy_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_RETRIGGER, f.content_left + policy_label_width,
          policy_top, half - policy_label_width, ui.scaled(90));
  const int recovery_left = f.content_left + half + ui.scaled(10);
  ui.move(IDC_COOLDOWN_RECOVERY_LABEL, recovery_left,
          policy_top + ui.scaled(3), policy_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_RECOVERY, recovery_left + policy_label_width,
          policy_top, f.content_right - recovery_left - policy_label_width,
          ui.scaled(100));

  const int groups_top = policy_top + ui.scaled(31);
  ui.move(IDC_COOLDOWN_GROUPS_LABEL, f.content_left, groups_top + ui.scaled(3),
          groups_label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_GROUPS, f.content_left + groups_label_width,
          groups_top, f.content_width - groups_label_width, f.row_height);
  return groups_top + f.row_height + ui.scaled(18);
}

// Binding row plus the advanced rows below it; returns the top of the bars
// heading.
int layout_cooldown_binding(const MainLayout &f, const int binding_top) {
  const DialogLayout &ui = f.ui;
  const int label_width = ui.scaled(58);
  const int capture_width = ui.scaled(104);
  const int hid_width = f.advanced ? ui.scaled(66) : 0;
  const int buttons_width =
      capture_width + (f.advanced ? ui.scaled(7) + hid_width : 0);
  ui.move(IDC_COOLDOWN_BINDING_LABEL, f.content_left,
          binding_top + ui.scaled(3), label_width, f.heading_height);
  ui.move(IDC_COOLDOWN_BINDING, f.content_left + label_width, binding_top,
          f.content_width - label_width - buttons_width - ui.scaled(8),
          f.row_height);
  ui.move(IDC_CAPTURE_BINDING, f.content_right - buttons_width, binding_top,
          capture_width, f.row_height);
  if (!f.advanced)
    return binding_top + f.row_height + ui.scaled(18);
  ui.move(IDC_CAPTURE_HID, f.content_right - hid_width, binding_top,
          hid_width, f.row_height);
  return layout_cooldown_technical(f, binding_top + ui.scaled(31),
                                   label_width);
}

// Four color swatches in one row, or a 2x2 grid when compact; returns the top
// of the checkbox row.
int layout_bar_swatches(const MainLayout &f, const int left, const int top,
                        const int width, const bool compact) {
  const DialogLayout &ui = f.ui;
  const int swatch_gap = ui.scaled(5);
  const int swatch_height = ui.scaled(27);
  if (compact) {
    const int swatch_width = (width - swatch_gap) / 2;
    const int second_left = left + swatch_width + swatch_gap;
    const int second_width = width - swatch_width - swatch_gap;
    ui.move(IDC_BAR_FILL, left, top, swatch_width, swatch_height);
    ui.move(IDC_BAR_BACKGROUND, second_left, top, second_width,
            swatch_height);
    const int second_top = top + ui.scaled(34);
    ui.move(IDC_BAR_BORDER, left, second_top, swatch_width, swatch_height);
    ui.move(IDC_BAR_TEXT, second_left, second_top, second_width,
            swatch_height);
    return top + ui.scaled(68);
  }
  const int swatch_width = (width - swatch_gap * 3) / 4;
  const int stride = swatch_width + swatch_gap;
  ui.move(IDC_BAR_FILL, left, top, swatch_width, swatch_height);
  ui.move(IDC_BAR_BACKGROUND, left + stride, top, swatch_width, swatch_height);
  ui.move(IDC_BAR_BORDER, left + stride * 2, top, swatch_width, swatch_height);
  ui.move(IDC_BAR_TEXT, left + stride * 3, top, width - stride * 3,
          swatch_height);
  return top + ui.scaled(34);
}

void layout_bar_properties(const MainLayout &f, const int left,
                           const int list_top, const int width,
                           const bool compact) {
  const DialogLayout &ui = f.ui;
  const int dimensions_top = list_top + ui.scaled(27);
  const int dimension_half = (width - ui.scaled(8)) / 2;
  ui.move(IDC_WIDTH_LABEL, left, dimensions_top + ui.scaled(3), ui.scaled(40),
          f.heading_height);
  ui.move(IDC_BAR_WIDTH, left + ui.scaled(44), dimensions_top,
          std::max(ui.scaled(42), dimension_half - ui.scaled(44)),
          f.row_height);
  const int height_left = left + dimension_half + ui.scaled(8);
  ui.move(IDC_HEIGHT_LABEL, height_left, dimensions_top + ui.scaled(3),
          ui.scaled(42), f.heading_height);
  ui.move(IDC_BAR_HEIGHT, height_left + ui.scaled(46), dimensions_top,
          std::max(ui.scaled(42), width - dimension_half - ui.scaled(54)),
          f.row_height);

  const int checkbox_top = layout_bar_swatches(
      f, left, list_top + ui.scaled(58), width, compact);
  const int checkbox_width = (width - ui.scaled(8)) / 2;
  ui.move(IDC_SHOW_TEXT, left, checkbox_top, checkbox_width, ui.scaled(25));
  ui.move(IDC_HIDE_READY, left + checkbox_width + ui.scaled(8), checkbox_top,
          width - checkbox_width - ui.scaled(8), ui.scaled(25));
  ui.move(IDC_DIRECT_DRAG, left, checkbox_top + ui.scaled(29), width,
          ui.scaled(25));
}

void layout_bars(const MainLayout &f, const int heading_top) {
  const DialogLayout &ui = f.ui;
  const int list_top = heading_top + f.heading_height + ui.scaled(4);
  const int list_width =
      std::clamp(f.content_width * 2 / 5, ui.scaled(185), ui.scaled(220));
  const int properties_left = f.content_left + list_width + f.gap;
  const int properties_width = f.content_width - list_width - f.gap;
  const bool compact_swatches = properties_width < ui.scaled(360);
  const int list_height = std::clamp(
      f.status_top - list_top - ui.scaled(40),
      ui.scaled(compact_swatches ? 184 : 118),
      ui.scaled(compact_swatches ? 196 : 150));
  ui.move(IDC_BARS_LABEL, f.content_left, heading_top, f.content_width,
          f.heading_height);
  ui.move(IDC_BAR_LIST, f.content_left, list_top, list_width, list_height);
  ui.move(IDC_SELECTED_BAR_LABEL, properties_left, list_top, properties_width,
          f.heading_height);
  layout_bar_properties(f, properties_left, list_top, properties_width,
                        compact_swatches);
  ui.move(IDC_POSITION_STATUS, f.content_left,
          list_top + list_height + ui.scaled(6), f.content_width,
          ui.scaled(26));
}

void layout_visual_panel(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  const int left = f.right_left;
  const int width = f.right_width;
  ui.move(IDC_VISUAL_SECTION_LABEL, left, f.section_top, width,
          f.heading_height);
  const int summary_top = f.section_top + f.heading_height + ui.scaled(4);
  ui.move(IDC_VISUAL_SUMMARY, left, summary_top, width - ui.scaled(90),
          ui.scaled(22));
  ui.move(IDC_VISUAL_ENABLED, left + width - ui.scaled(84), summary_top,
          ui.scaled(84), ui.scaled(22));
  const int actions_top = summary_top + ui.scaled(28);
  ui.move(IDC_CALIBRATE_VISUAL, left, actions_top, ui.scaled(112),
          ui.scaled(27));
  ui.move(IDC_REMOVE_VISUAL, left + ui.scaled(120), actions_top, ui.scaled(78),
          ui.scaled(27));
  if (f.advanced)
    ui.move(IDC_VISUAL_ISOLATED, left + ui.scaled(206), actions_top,
            width - ui.scaled(206), ui.scaled(27));
  const int status_top = actions_top + ui.scaled(34);
  ui.move(IDC_VISUAL_RUNTIME_STATUS, left, status_top, width, ui.scaled(36));

  const int preview_heading_top = status_top + ui.scaled(45);
  const int position_width = ui.scaled(154);
  ui.move(IDC_PREVIEW_LABEL, left, preview_heading_top,
          width - position_width - ui.scaled(8), f.heading_height);
  ui.move(IDC_POSITION_ON_WINDOW, left + width - position_width,
          preview_heading_top - ui.scaled(4), position_width, ui.scaled(27));
  const int preview_top = preview_heading_top + ui.scaled(30);
  const int preview_height =
      std::clamp(width * 3 / 4, ui.scaled(220), ui.scaled(340));
  ui.move(IDC_PREVIEW, left, preview_top, width,
          std::min(preview_height,
                   std::max(ui.scaled(120),
                            f.status_top - preview_top - ui.scaled(8))));
}

} // namespace

void EditorWindow::layout() {
  if (shell_.window == nullptr)
    return;
  const MainLayout frame =
      make_main_layout(shell_.window, shell_.advanced_visible);
  show_advanced_controls(shell_.window, frame.advanced);
  layout_sidebar(frame);
  const int binding_top =
      layout_cooldown_list(frame, layout_profile_fields(frame));
  layout_bars(frame, layout_cooldown_binding(frame, binding_top));
  layout_visual_panel(frame);

  const DialogLayout &ui = frame.ui;
  ui.move(IDC_STATUS, frame.margin, frame.status_top,
          frame.width - frame.margin * 2, ui.scaled(19));
  ListView_SetColumnWidth(shell_.profile_list, 0,
                          std::max(1, frame.sidebar_width - ui.scaled(8)));
  RedrawWindow(shell_.window, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

} // namespace layerc::editor
