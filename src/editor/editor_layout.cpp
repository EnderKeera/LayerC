#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_layout_rules.hpp"
#include "editor_resource.h"
#include "editor_text.hpp"

#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include <commctrl.h>
#include <windows.h>
namespace layerc::editor {
namespace {

// Controls that exist only in advanced mode.
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

// The bar list only appears while a legacy grouped bar exists; otherwise the
// overlay bar follows the selected cooldown.
constexpr std::array legacy_bar_controls{IDC_BARS_LABEL, IDC_BAR_LIST};
constexpr std::array preview_controls{IDC_PREVIEW_LABEL, IDC_PREVIEW};
// Lines that show only while they have text.
constexpr std::array text_lines{IDC_COOLDOWN_REMOVE_REASON, IDC_ITEM_REASON,
                                IDC_OVERLAY_CONTRAST};

constexpr std::array heading_controls{
    IDC_SAVED_PROFILES_LABEL, IDC_COOLDOWNS_LABEL,  IDC_TIMING_LABEL,
    IDC_OVERLAY_LABEL,        IDC_VISUAL_SECTION_LABEL, IDC_PREVIEW_LABEL,
    IDC_BARS_LABEL, IDC_PANEL_ITEMS_LABEL};

// Logical (96-DPI, 100 % text) metrics: one control height per surface and
// an 8 px rhythm.
constexpr int control_height = native_control_height;
constexpr int row_pitch = control_height + 8;
constexpr int heading_pitch = 28;
constexpr int section_gap = 12;
constexpr int field_cap = 480;
constexpr int preview_threshold = 1100;
constexpr int slot_min = 260;
constexpr int sidebar_min = 200;
constexpr int sidebar_max = 280;
constexpr int cooldown_list_min = 84;
constexpr int timing_label_width = 84;

// The main window (DesignSystem.md, Layout): a top band of profile fields,
// the cooldown list and the preview corner, over three equal settings slots
// anchored to the status strip. `preview` is the wide form; without it the
// slots stack in one or two columns.
struct MainLayout {
  DialogLayout ui;
  HWND window{};
  bool advanced{};
  bool legacy_bars{};
  bool preview{};
  int width{};
  int height{};
  int margin{};
  int gap{};
  int row{};
  int pitch{};
  int heading{};
  int status_top{};
  int sidebar_width{};
  int detail_left{};
  int detail_right{};
  int preview_left{};
  int slot{};
  int wide_minimum{};
};

MainLayout make_main_layout(const HWND window, const bool advanced,
                            const bool legacy_bars,
                            const int requested_width = 0) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = requested_width > 0
                        ? requested_width
                        : std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  // Geometry follows DPI and text size and never shrinks below the text:
  // narrow windows reflow (the preview corner hides) instead.
  const DialogLayout ui(window, window_base_dpi(window));
  MainLayout f{.ui = ui,
               .window = window,
               .advanced = advanced,
               .legacy_bars = legacy_bars,
               .width = width,
               .height = height,
               .margin = ui.scaled(12),
               .gap = ui.scaled(16),
               .row = ui.scaled(control_height),
               .pitch = ui.scaled(row_pitch),
               .heading = ui.scaled(heading_pitch),
               .status_top = height - ui.scaled(28)};
  f.sidebar_width =
      std::clamp(width / 5, ui.scaled(sidebar_min), ui.scaled(sidebar_max));
  // Enough for all three slots even at the sidebar's widest, using the
  // same metrics as placement rather than a separate resize threshold.
  f.wide_minimum = std::max(ui.scaled(preview_threshold),
                            f.margin * 2 + ui.scaled(sidebar_max) + f.gap * 3 +
                                ui.scaled(slot_min) * 3);
  f.detail_left = f.margin + f.sidebar_width + f.gap;
  f.slot = main_slot_width(width, f.margin, f.detail_left, f.gap);
  f.preview = main_layout_wide(width, f.slot, ui.scaled(preview_threshold),
                               ui.scaled(slot_min));
  if (f.preview) {
    // The preview corner is slot 3's width and sits right over it.
    f.preview_left = f.detail_left + (f.slot + f.gap) * 2;
    f.detail_right = f.preview_left - f.gap;
  } else {
    f.preview_left = width;
    f.detail_right = width - f.margin;
  }
  return f;
}

void show_controls(const HWND window, const auto &ids, const bool visible) {
  for (const int id : ids) {
    if (const HWND control = GetDlgItem(window, id); control != nullptr)
      ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
  }
}

// A label vertically centered on a control row.
void label(const MainLayout &f, const int id, const int left, const int top,
           const int width) {
  f.ui.row_label(id, left, top, width, f.row);
}

// A hint, reason or warning line's height wrapped to `width`; 0 while it has
// no text, so an empty line takes no room.
int line_height(const MainLayout &f, const int id, const int width) {
  const HWND control = GetDlgItem(f.window, id);
  if (control == nullptr || GetWindowTextLengthW(control) == 0)
    return 0;
  return static_cast<int>(control_text_extent(control, std::max(1, width)).cy);
}

// The overlay contrast line leads with a drawn glyph; its text wraps in the
// width that leaves.
int contrast_line_height(const MainLayout &f, const int width) {
  const int glyph =
      layerc::platform::status_glyph_size(
          layerc::platform::native_window_dpi(f.window)) +
      f.ui.scaled(6);
  return line_height(f, IDC_OVERLAY_CONTRAST, width - glyph);
}

// The look slot's closing lines: the drag hint, then the contrast line.
int overlay_lines_height(const MainLayout &f, const int width) {
  int height = f.ui.scaled(4) + line_height(f, IDC_POSITION_STATUS, width);
  if (const int contrast = contrast_line_height(f, width); contrast > 0)
    height += f.ui.scaled(4) + contrast;
  return height;
}

void layout_sidebar(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_SAVED_PROFILES_LABEL, f.margin, f.margin, f.sidebar_width,
          ui.scaled(20));
  const int list_top = f.margin + f.heading;
  // Keep panel items visible without making the main form taller. The two
  // sidebar lists share the available height; their controls never overlap.
  const int bottom = f.status_top - f.margin;
  const int profile_height = std::max(
      ui.scaled(80), (bottom - list_top - f.heading - f.pitch * 3 -
                     ui.scaled(16)) / 2);
  ui.move(IDC_PROFILE_LIST, f.margin, list_top, f.sidebar_width, profile_height);
  const int buttons_top = list_top + profile_height + ui.scaled(8);
  const int button = ui.scaled(36);
  int left = f.margin;
  for (const int id : {IDC_PROFILE_NEW, IDC_PROFILE_DELETE, IDC_PROFILE_MORE}) {
    ui.move(id, left, buttons_top, button, f.row);
    left += button + ui.scaled(4);
  }
  const int heading_top = buttons_top + f.pitch + ui.scaled(8);
  ui.move(IDC_PANEL_ITEMS_LABEL, f.margin, heading_top, f.sidebar_width,
          ui.scaled(20));
  const int item_top = heading_top + f.heading;
  // Why an item button is unavailable, under the buttons it explains.
  const int reason = line_height(f, IDC_ITEM_REASON, f.sidebar_width);
  const int actions_top =
      bottom - f.pitch - f.row - (reason > 0 ? reason + ui.scaled(4) : 0);
  ui.move(IDC_PANEL_ITEMS, f.margin, item_top, f.sidebar_width,
          std::max(ui.scaled(40), actions_top - ui.scaled(8) - item_top));
  const int third = (f.sidebar_width - ui.scaled(16)) / 3;
  left = f.margin;
  for (const int id : {IDC_ITEM_ADD, IDC_ITEM_EDIT, IDC_ITEM_REMOVE}) {
    ui.move(id, left, actions_top, third, f.row);
    left += third + ui.scaled(8);
  }
  const int half = (f.sidebar_width - ui.scaled(8)) / 2;
  ui.move(IDC_ITEM_UP, f.margin, actions_top + f.pitch, half, f.row);
  ui.move(IDC_ITEM_DOWN, f.margin + half + ui.scaled(8),
          actions_top + f.pitch, f.sidebar_width - half - ui.scaled(8), f.row);
  ui.move(IDC_ITEM_REASON, f.margin,
          actions_top + f.pitch + f.row + ui.scaled(4), f.sidebar_width,
          std::max(1, reason));
}

// Title, profile fields and the advanced toggle; returns the next top.
int layout_profile(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  const int width = f.detail_right - f.detail_left;
  const int toggle_width = ui.scaled(132);
  ui.move(IDC_ADVANCED, f.detail_right - toggle_width, f.margin, toggle_width,
          f.row);
  ui.move(IDC_PROFILE_TITLE, f.detail_left, f.margin,
          std::max(1, width - toggle_width - ui.scaled(8)), f.row);
  const int label_width = ui.scaled(96);
  const int field_left = f.detail_left + label_width;
  const int field_width =
      std::min(ui.scaled(field_cap), f.detail_right - field_left);
  int top = f.margin + f.row + ui.scaled(10);
  label(f, IDC_PROFILE_NAME_LABEL, f.detail_left, top, label_width - ui.scaled(8));
  ui.move(IDC_PROFILE_NAME, field_left, top, field_width, f.row);
  top += f.pitch;
  if (f.advanced) {
    label(f, IDC_PROFILE_ID_LABEL, f.detail_left, top, label_width - ui.scaled(8));
    ui.move(IDC_PROFILE_ID, field_left, top, field_width, f.row);
    top += f.pitch;
  }
  const int browse_width = ui.scaled(88);
  const int choose_width = ui.scaled(150);
  const int buttons = browse_width + choose_width + ui.scaled(16);
  const int application_width = std::max(
      ui.scaled(120),
      std::min(ui.scaled(field_cap) - buttons, f.detail_right - field_left - buttons));
  label(f, IDC_APPLICATION_LABEL, f.detail_left, top, label_width - ui.scaled(8));
  ui.move(IDC_APPLICATION_PATH, field_left, top, application_width, f.row);
  int left = field_left + application_width + ui.scaled(8);
  ui.move(IDC_BROWSE_APPLICATION, left, top, browse_width, f.row);
  left += browse_width + ui.scaled(8);
  ui.move(IDC_CHOOSE_APPLICATION, left, top, choose_width, f.row);
  top += f.pitch;
  if (f.advanced) {
    ui.move(IDC_APPLICATION_EXACT_PATH, field_left, top, field_width,
            ui.scaled(24));
    top += f.row;
  }
  return top + ui.scaled(4);
}

struct GroupHeights {
  int timing{};
  int visual{};
  int overlay{};
};

int capture_buttons_width(const MainLayout &f) {
  return f.ui.scaled(100) + (f.advanced ? f.ui.scaled(60 + 8) : 0);
}

// Capture key (and HID) share the binding row as in the editor shell; only a
// timing column too narrow for the field floor wraps them below the binding.
bool binding_wraps(const MainLayout &f, const int timing_width) {
  return capture_buttons_wrap(timing_width, f.ui.scaled(timing_label_width),
                              capture_buttons_width(f), f.ui.scaled(8),
                              f.ui.scaled(binding_field_floor));
}

GroupHeights group_heights(const MainLayout &f, const int timing_width,
                           const int overlay_width) {
  const int advanced_rows = f.advanced ? 4 : 0;
  const int wrapped_rows = binding_wraps(f, timing_width) ? 1 : 0;
  return {
      .timing = f.heading + f.pitch * (4 + advanced_rows + wrapped_rows),
      .visual = f.heading + f.pitch * (f.advanced ? 3 : 2) + f.ui.scaled(40),
      // Size, two color rows, the two check rows, Position on window when
      // the preview corner is hidden, then the closing lines.
      .overlay = f.heading + (f.legacy_bars ? f.ui.scaled(24 + 80 + 8) : 0) +
                 f.pitch * 3 + f.row * 2 +
                 (f.preview ? 0 : f.ui.scaled(4) + f.row) +
                 overlay_lines_height(f, overlay_width),
  };
}

// Returns the top of the next row.
int layout_timing(const MainLayout &f, const int left, const int right,
                  int top) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_TIMING_LABEL, left, top, right - left, ui.scaled(20));
  top += f.heading;
  const int label_width = ui.scaled(timing_label_width);
  const int field_left = left + label_width;
  const int field_width = std::max(ui.scaled(60), right - field_left);
  label(f, IDC_COOLDOWN_NAME_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_NAME, field_left, top, field_width, f.row);
  top += f.pitch;
  label(f, IDC_COOLDOWN_DURATION_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_DURATION, field_left, top, ui.scaled(80), f.row);
  top += f.pitch;
  label(f, IDC_COOLDOWN_ACTIVE_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_ACTIVE, field_left, top, ui.scaled(80), f.row);
  label(f, IDC_COOLDOWN_ACTIVE_HINT, field_left + ui.scaled(88), top,
        std::max(1, right - field_left - ui.scaled(88)));
  top += f.pitch;
  const int capture_width = ui.scaled(100);
  const int hid_width = f.advanced ? ui.scaled(60) : 0;
  const int buttons = capture_buttons_width(f);
  label(f, IDC_COOLDOWN_BINDING_LABEL, left, top, label_width - ui.scaled(8));
  if (binding_wraps(f, right - left)) {
    ui.move(IDC_COOLDOWN_BINDING, field_left, top, field_width, f.row);
    top += f.pitch;
    ui.move(IDC_CAPTURE_BINDING, field_left, top, capture_width, f.row);
    if (f.advanced)
      ui.move(IDC_CAPTURE_HID, field_left + capture_width + ui.scaled(8), top,
              hid_width, f.row);
  } else {
    ui.move(IDC_COOLDOWN_BINDING, field_left, top,
            right - field_left - buttons - ui.scaled(8), f.row);
    ui.move(IDC_CAPTURE_BINDING, right - buttons, top, capture_width, f.row);
    if (f.advanced)
      ui.move(IDC_CAPTURE_HID, right - hid_width, top, hid_width, f.row);
  }
  top += f.pitch;
  if (!f.advanced)
    return top;
  const int charges_width = ui.scaled(56);
  const int charges_label = ui.scaled(64);
  label(f, IDC_COOLDOWN_ID_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_ID, field_left, top,
          std::max(ui.scaled(40), right - field_left - charges_width -
                                      charges_label - ui.scaled(8)),
          f.row);
  label(f, IDC_COOLDOWN_CHARGES_LABEL, right - charges_width - charges_label,
        top, charges_label - ui.scaled(6));
  ui.move(IDC_COOLDOWN_CHARGES, right - charges_width, top, charges_width,
          f.row);
  top += f.pitch;
  label(f, IDC_COOLDOWN_RETRIGGER_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_RETRIGGER, field_left, top, field_width, ui.scaled(120));
  top += f.pitch;
  label(f, IDC_COOLDOWN_RECOVERY_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_RECOVERY, field_left, top, field_width, ui.scaled(140));
  top += f.pitch;
  label(f, IDC_COOLDOWN_GROUPS_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_GROUPS, field_left, top, field_width, f.row);
  return top + f.pitch;
}

// `bottom` is the settings row's bottom line: the hint takes what is left.
void layout_visual(const MainLayout &f, const int left, const int right,
                   int top, const int bottom) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_VISUAL_SECTION_LABEL, left, top, right - left, ui.scaled(20));
  top += f.heading;
  const int enabled_width = ui.scaled(96);
  ui.move(IDC_VISUAL_SUMMARY, left, top + (f.row - ui.scaled(20)) / 2,
          std::max(1, right - left - enabled_width - ui.scaled(8)),
          ui.scaled(20));
  ui.move(IDC_VISUAL_ENABLED, right - enabled_width, top, enabled_width, f.row);
  top += f.pitch;
  const int calibrate_width = ui.scaled(104);
  ui.move(IDC_CALIBRATE_VISUAL, left, top, calibrate_width, f.row);
  ui.move(IDC_REMOVE_VISUAL, left + calibrate_width + ui.scaled(8), top,
          ui.scaled(124), f.row);
  top += f.pitch;
  if (f.advanced) {
    ui.move(IDC_VISUAL_ISOLATED, left, top, right - left, f.row);
    top += f.pitch;
  }
  ui.move(IDC_VISUAL_HINT, left, top, right - left,
          std::max(ui.scaled(40), bottom - top));
}

void layout_overlay(const MainLayout &f, const int left, const int right,
                    int top) {
  const DialogLayout &ui = f.ui;
  const int width = right - left;
  ui.move(IDC_OVERLAY_LABEL, left, top, width, ui.scaled(20));
  top += f.heading;
  if (f.legacy_bars) {
    ui.move(IDC_BARS_LABEL, left, top, width, ui.scaled(20));
    top += ui.scaled(24);
    ui.move(IDC_BAR_LIST, left, top, width, ui.scaled(80));
    top += ui.scaled(88);
  }
  const int half = (width - ui.scaled(8)) / 2;
  const int label_width = ui.scaled(56);
  const int number_width = std::min(ui.scaled(80), half - label_width);
  label(f, IDC_WIDTH_LABEL, left, top, label_width - ui.scaled(6));
  ui.move(IDC_BAR_WIDTH, left + label_width, top, number_width, f.row);
  const int second = left + half + ui.scaled(8);
  label(f, IDC_HEIGHT_LABEL, second, top, label_width - ui.scaled(6));
  ui.move(IDC_BAR_HEIGHT, second + label_width, top, number_width, f.row);
  top += f.pitch;
  ui.move(IDC_BAR_FILL, left, top, half, f.row);
  ui.move(IDC_BAR_BACKGROUND, second, top, width - half - ui.scaled(8), f.row);
  top += f.pitch;
  ui.move(IDC_BAR_BORDER, left, top, half, f.row);
  ui.move(IDC_BAR_TEXT, second, top, width - half - ui.scaled(8), f.row);
  top += f.pitch;
  ui.move(IDC_SHOW_TEXT, left, top, half, f.row);
  ui.move(IDC_HIDE_READY, second, top, width - half - ui.scaled(8), f.row);
  top += f.row;
  ui.move(IDC_DIRECT_DRAG, left, top, width, f.row);
  top += f.row;
  if (!f.preview) {
    // No preview corner to carry it: Position on window joins the look.
    top += ui.scaled(4);
    ui.move(IDC_POSITION_ON_WINDOW, left, top,
            std::min(width, ui.scaled(180)), f.row);
    top += f.row;
  }
  top += ui.scaled(4);
  const int hint = line_height(f, IDC_POSITION_STATUS, width);
  ui.move(IDC_POSITION_STATUS, left, top, width, std::max(1, hint));
  top += hint + ui.scaled(4);
  ui.move(IDC_OVERLAY_CONTRAST, left, top, width,
          std::max(1, contrast_line_height(f, width)));
}

// The preview corner over slot 3: its heading and Position on window, then
// the frame down to the list's bottom line.
void layout_preview(const MainLayout &f, const int bottom) {
  if (!f.preview)
    return;
  const DialogLayout &ui = f.ui;
  const int width = f.width - f.margin - f.preview_left;
  const int button_width = std::min(ui.scaled(180), width / 2);
  ui.move(IDC_PREVIEW_LABEL, f.preview_left, f.margin,
          width - button_width - ui.scaled(8), ui.scaled(20));
  ui.move(IDC_POSITION_ON_WINDOW, f.preview_left + width - button_width,
          f.margin, button_width, f.row);
  const int top = f.margin + f.row + ui.scaled(10);
  ui.move(IDC_PREVIEW, f.preview_left, top, width, std::max(1, bottom - top));
}

struct CooldownGroups {
  bool two_columns{};
  int column{};
  int overlay_left{};
  GroupHeights heights;
  int height{};
};

// Measurement and placement share the same reflow and wrapped-text extents.
CooldownGroups cooldown_groups(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  const int row_right = f.width - f.margin;
  const int row_width = row_right - f.detail_left;
  const bool two_columns = !f.preview && row_width >= ui.scaled(480);
  const int column = (row_width - f.gap) / 2;
  const int timing_width = f.preview ? f.slot : two_columns ? column : row_width;
  const int overlay_left = f.preview      ? f.preview_left
                           : two_columns ? f.detail_left + column + f.gap
                                         : f.detail_left;
  const auto heights = group_heights(f, timing_width, row_right - overlay_left);
  const int height =
      f.preview ? std::max({heights.timing, heights.visual, heights.overlay})
      : two_columns
          ? std::max(heights.timing + ui.scaled(section_gap) + heights.visual,
                     heights.overlay)
          : heights.timing + heights.visual + heights.overlay +
                ui.scaled(section_gap) * 2;
  return {two_columns, column, overlay_left, heights, height};
}

int cooldown_minimum_bottom(const MainLayout &f, const int top,
                             const CooldownGroups &groups) {
  return top + f.heading + f.ui.scaled(cooldown_list_min) + f.ui.scaled(16) +
         groups.height;
}

// The cooldown list and the preview corner, then the selected cooldown's
// settings row: three slots when wide, else two columns (one when narrow).
// The row is anchored to the status strip and the list takes the rest.
// Returns the height the detail column needs.
int layout_cooldowns(const MainLayout &f, const int top) {
  const DialogLayout &ui = f.ui;
  const int row_right = f.width - f.margin;
  const auto groups = cooldown_groups(f);
  const auto &[two_columns, column, overlay_left, heights, groups_height] = groups;
  const int list_top = top + f.heading;
  const int min_list = ui.scaled(cooldown_list_min);
  const int groups_top = main_groups_top(list_top, min_list, ui.scaled(16),
                                         f.status_top - f.margin, groups_height);
  const int list_height = groups_top - ui.scaled(16) - list_top;
  const int width = f.detail_right - f.detail_left;
  const int button_width = ui.scaled(150);
  const int list_width = std::min(ui.scaled(560),
                                  width - button_width - ui.scaled(8));
  ui.move(IDC_COOLDOWNS_LABEL, f.detail_left, top, list_width, ui.scaled(20));
  ui.move(IDC_COOLDOWN_LIST, f.detail_left, list_top, list_width, list_height);
  const int button_left = f.detail_left + list_width + ui.scaled(8);
  ui.move(IDC_ADD_COOLDOWN, button_left, list_top, button_width, f.row);
  ui.move(IDC_REMOVE_COOLDOWN, button_left, list_top + f.pitch, button_width,
          f.row);
  // Why Remove cooldown is unavailable, under it and above the settings row.
  const int reason_top = list_top + f.pitch + f.row + ui.scaled(4);
  ui.move(IDC_COOLDOWN_REMOVE_REASON, button_left, reason_top, button_width,
          std::max(1, groups_top - reason_top));
  layout_preview(f, list_top + list_height);

  const int groups_bottom = groups_top + groups_height;
  if (f.preview) {
    const int second = f.detail_left + f.slot + f.gap;
    (void)layout_timing(f, f.detail_left, f.detail_left + f.slot, groups_top);
    layout_visual(f, second, second + f.slot, groups_top, groups_bottom);
    layout_overlay(f, overlay_left, row_right, groups_top);
  } else if (two_columns) {
    const int left_right = f.detail_left + column;
    const int visual_top =
        layout_timing(f, f.detail_left, left_right, groups_top) +
        ui.scaled(section_gap);
    layout_visual(f, f.detail_left, left_right, visual_top,
                  visual_top + heights.visual);
    layout_overlay(f, overlay_left, row_right, groups_top);
  } else {
    const int visual_top =
        layout_timing(f, f.detail_left, row_right, groups_top) +
        ui.scaled(section_gap);
    layout_visual(f, f.detail_left, row_right, visual_top,
                  visual_top + heights.visual);
    layout_overlay(f, f.detail_left, row_right,
                   visual_top + heights.visual + ui.scaled(section_gap));
  }
  return cooldown_minimum_bottom(f, top, groups);
}

void layout_status(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  const int text_top = f.status_top + ui.scaled(5);
  // Each state leads with its glyph.
  const int connection_width = ui.scaled(190);
  const int runtime_width = ui.scaled(270);
  const int right = f.width - f.margin;
  ui.move(IDC_CONNECTION_STATUS, right - connection_width, text_top,
          connection_width, ui.scaled(20));
  ui.move(IDC_VISUAL_RUNTIME_STATUS,
          right - connection_width - runtime_width - ui.scaled(12), text_top,
          runtime_width, ui.scaled(20));
  ui.move(IDC_STATUS, f.margin, text_top,
          std::max(1, right - connection_width - runtime_width -
                          ui.scaled(24) - f.margin),
          ui.scaled(20));
}

} // namespace

void EditorWindow::set_sized_line(const int id, const std::wstring_view text) {
  const HWND line = GetDlgItem(shell_.window, id);
  if (line == nullptr || layerc::platform::window_text(line) == text)
    return;
  set_text(line, text);
  if (!shell_.laying_out && IsIconic(shell_.window) == FALSE)
    layout();
}

void EditorWindow::apply_heading_fonts() {
  if (shell_.window == nullptr)
    return;
  for (const int id : heading_controls) {
    if (const HWND control = GetDlgItem(shell_.window, id);
        control != nullptr && shell_.heading_font != nullptr)
      SendMessageW(control, WM_SETFONT,
                   reinterpret_cast<WPARAM>(shell_.heading_font), FALSE);
  }
  if (const HWND title = GetDlgItem(shell_.window, IDC_PROFILE_TITLE);
      title != nullptr && shell_.title_font != nullptr)
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(shell_.title_font),
                 FALSE);
}

void EditorWindow::layout() {
  if (shell_.window == nullptr)
    return;
  const bool legacy_bars = std::ranges::any_of(
      document_.profile().panels, [](const layerc::core::Panel &panel) {
        return panel.items.has_value() ||
               panel.style != layerc::core::PanelStyle::bar ||
               panel.cooldown_ids.size() > 1;
      });
  const MainLayout frame =
      make_main_layout(shell_.window, shell_.advanced_visible, legacy_bars);
  // Lines resized below must not re-enter through set_sized_line().
  shell_.laying_out = true;
  // The drag hint names the preview only while it shows, and its height
  // sizes the look slot.
  if (shell_.preview_visible != frame.preview) {
    shell_.preview_visible = frame.preview;
    refresh_position_hint();
  }
  show_controls(shell_.window, advanced_controls, frame.advanced);
  show_controls(shell_.window, legacy_bar_controls, frame.legacy_bars);
  show_controls(shell_.window, preview_controls, frame.preview);
  for (const int id : text_lines) {
    if (const HWND line = GetDlgItem(shell_.window, id); line != nullptr)
      ShowWindow(line, GetWindowTextLengthW(line) > 0 ? SW_SHOWNA : SW_HIDE);
  }
  // Tab order is reading order: Position on window follows the preview
  // heading in the corner, or Direct drag once it moves into the look slot.
  SetWindowPos(GetDlgItem(shell_.window, IDC_POSITION_ON_WINDOW),
               GetDlgItem(shell_.window,
                          frame.preview ? IDC_PREVIEW_LABEL : IDC_DIRECT_DRAG),
               0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  layout_sidebar(frame);
  const int profile_bottom = layout_profile(frame);
  const int detail_bottom = layout_cooldowns(frame, profile_bottom);
  layout_status(frame);
  place_button_reason_tools(shell_);
  shell_.status_strip_top = frame.status_top;
  // The smallest client height that keeps every row visible.
  shell_.min_client_height = detail_bottom + frame.margin +
                             (frame.height - frame.status_top);
  const SIZE work = work_area_client_size(shell_.window);
  const int narrow_width = frame.ui.scaled(main_client_min_width);
  const MainLayout narrow = make_main_layout(
      shell_.window, frame.advanced, frame.legacy_bars,
      static_cast<int>(std::min<LONG>(narrow_width, work.cx)));
  const int narrow_height =
      cooldown_minimum_bottom(narrow, profile_bottom, cooldown_groups(narrow)) +
      narrow.margin + (narrow.height - narrow.status_top);
  // If stacking cannot fit vertically, retain the three-slot layout. Merely
  // capping the height would leave the visual controls below the screen.
  shell_.min_client_width = main_minimum_width(
      narrow_width, narrow_height, frame.wide_minimum, work.cx, work.cy);
  shell_.laying_out = false;

  RECT list_client{};
  GetClientRect(shell_.profile_list, &list_client);
  ListView_SetColumnWidth(shell_.profile_list, 0,
                          std::max(1L, list_client.right - list_client.left));
  RedrawWindow(shell_.window, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  // Last: growing re-enters layout() through WM_SIZE at the new extent.
  grow_client_to(shell_.window,
                 {shell_.min_client_width,
                  std::min<LONG>(shell_.min_client_height, work.cy)});
}

} // namespace layerc::editor
