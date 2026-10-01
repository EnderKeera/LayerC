#include "editor_window.hpp"
#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>

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

constexpr std::array heading_controls{
    IDC_SAVED_PROFILES_LABEL, IDC_COOLDOWNS_LABEL,  IDC_TIMING_LABEL,
    IDC_OVERLAY_LABEL,        IDC_VISUAL_SECTION_LABEL, IDC_PREVIEW_LABEL,
    IDC_BARS_LABEL};

// Logical (96-DPI, 100 % text) metrics: one control height per surface and
// an 8 px rhythm.
constexpr int control_height = 30;
constexpr int row_pitch = 38;
constexpr int heading_pitch = 28;
constexpr int section_gap = 12;
constexpr int field_cap = 480;
constexpr int preview_threshold = 1100;

struct MainLayout {
  DialogLayout ui;
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
};

MainLayout make_main_layout(const HWND window, const bool advanced,
                            const bool legacy_bars) {
  RECT client{};
  GetClientRect(window, &client);
  const int width = std::max(1, static_cast<int>(client.right - client.left));
  const int height = std::max(1, static_cast<int>(client.bottom - client.top));
  // Geometry follows DPI and text size and never shrinks below the text:
  // narrow windows reflow (the preview column collapses) instead.
  const DialogLayout ui(window, window_base_dpi(window));
  MainLayout f{.ui = ui,
               .advanced = advanced,
               .legacy_bars = legacy_bars,
               .preview = width >= ui.scaled(preview_threshold),
               .width = width,
               .height = height,
               .margin = ui.scaled(12),
               .gap = ui.scaled(16),
               .row = ui.scaled(control_height),
               .pitch = ui.scaled(row_pitch),
               .heading = ui.scaled(heading_pitch),
               .status_top = height - ui.scaled(28)};
  f.sidebar_width = std::clamp(width / 5, ui.scaled(200), ui.scaled(280));
  f.detail_left = f.margin + f.sidebar_width + f.gap;
  if (f.preview) {
    const int preview_width =
        std::clamp(width * 26 / 100, ui.scaled(280), ui.scaled(440));
    f.preview_left = width - f.margin - preview_width;
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
  f.ui.move(id, left, top + (f.row - f.ui.scaled(20)) / 2, width,
            f.ui.scaled(20));
}

void layout_sidebar(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_SAVED_PROFILES_LABEL, f.margin, f.margin, f.sidebar_width,
          ui.scaled(20));
  const int list_top = f.margin + f.heading;
  const int buttons_top = f.status_top - f.margin - f.row;
  ui.move(IDC_PROFILE_LIST, f.margin, list_top, f.sidebar_width,
          std::max(ui.scaled(80), buttons_top - ui.scaled(8) - list_top));
  const int button = ui.scaled(36);
  int left = f.margin;
  for (const int id : {IDC_PROFILE_NEW, IDC_PROFILE_DELETE, IDC_PROFILE_MORE}) {
    ui.move(id, left, buttons_top, button, f.row);
    left += button + ui.scaled(4);
  }
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
    top += ui.scaled(30);
  }
  return top + ui.scaled(4);
}

struct GroupHeights {
  int timing{};
  int visual{};
  int overlay{};
};

GroupHeights group_heights(const MainLayout &f) {
  const int advanced_rows = f.advanced ? 4 : 0;
  return {
      .timing = f.heading + f.pitch * (3 + advanced_rows),
      .visual = f.heading + f.pitch * (f.advanced ? 3 : 2) + f.ui.scaled(40),
      .overlay = f.heading + (f.legacy_bars ? f.ui.scaled(24 + 80 + 8) : 0) +
                 f.pitch * 3 + f.ui.scaled(30) * 2 + f.ui.scaled(60) +
                 (f.preview ? 0 : f.pitch),
  };
}

// Returns the top of the next row.
int layout_timing(const MainLayout &f, const int left, const int right,
                  int top) {
  const DialogLayout &ui = f.ui;
  ui.move(IDC_TIMING_LABEL, left, top, right - left, ui.scaled(20));
  top += f.heading;
  const int label_width = ui.scaled(84);
  const int field_left = left + label_width;
  const int field_width = std::max(ui.scaled(60), right - field_left);
  label(f, IDC_COOLDOWN_NAME_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_NAME, field_left, top, field_width, f.row);
  top += f.pitch;
  label(f, IDC_COOLDOWN_DURATION_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_DURATION, field_left, top, ui.scaled(80), f.row);
  top += f.pitch;
  const int capture_width = ui.scaled(100);
  const int hid_width = f.advanced ? ui.scaled(60) : 0;
  const int buttons =
      capture_width + (f.advanced ? hid_width + ui.scaled(8) : 0);
  label(f, IDC_COOLDOWN_BINDING_LABEL, left, top, label_width - ui.scaled(8));
  ui.move(IDC_COOLDOWN_BINDING, field_left, top,
          std::max(ui.scaled(60), right - field_left - buttons - ui.scaled(8)),
          f.row);
  ui.move(IDC_CAPTURE_BINDING, right - buttons, top, capture_width, f.row);
  if (f.advanced)
    ui.move(IDC_CAPTURE_HID, right - hid_width, top, hid_width, f.row);
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

void layout_visual(const MainLayout &f, const int left, const int right,
                   int top) {
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
  ui.move(IDC_VISUAL_HINT, left, top, right - left, ui.scaled(40));
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
  top += ui.scaled(30);
  ui.move(IDC_DIRECT_DRAG, left, top, width, f.row);
  top += ui.scaled(30);
  ui.move(IDC_POSITION_STATUS, left, top, width, ui.scaled(60));
  top += ui.scaled(60);
  if (!f.preview)
    ui.move(IDC_POSITION_ON_WINDOW, left, top + ui.scaled(4),
            std::min(width, ui.scaled(180)), f.row);
}

// Cooldowns, then the selected cooldown's detail in two columns (one when
// narrow). Returns the height the detail column needs.
int layout_cooldowns(const MainLayout &f, const int top) {
  const DialogLayout &ui = f.ui;
  const int width = f.detail_right - f.detail_left;
  const auto heights = group_heights(f);
  const bool two_columns = width >= ui.scaled(480);
  const int groups_height =
      two_columns ? std::max(heights.timing + ui.scaled(section_gap) +
                                 heights.visual,
                             heights.overlay)
                  : heights.timing + heights.visual + heights.overlay +
                        ui.scaled(section_gap) * 2;
  const int list_top = top + f.heading;
  const int min_list = ui.scaled(84);
  const int max_list = ui.scaled(180);
  const int list_height =
      std::clamp(f.status_top - f.margin - groups_height - ui.scaled(16) -
                     list_top,
                 min_list, max_list);
  const int button_width = ui.scaled(150);
  const int list_width = std::min(ui.scaled(560),
                                  width - button_width - ui.scaled(8));
  ui.move(IDC_COOLDOWNS_LABEL, f.detail_left, top, list_width, ui.scaled(20));
  ui.move(IDC_COOLDOWN_LIST, f.detail_left, list_top, list_width, list_height);
  const int button_left = f.detail_left + list_width + ui.scaled(8);
  ui.move(IDC_ADD_COOLDOWN, button_left, list_top, button_width, f.row);
  ui.move(IDC_REMOVE_COOLDOWN, button_left, list_top + f.pitch, button_width,
          f.row);

  const int groups_top = list_top + list_height + ui.scaled(16);
  if (two_columns) {
    const int column = (width - f.gap) / 2;
    const int left_right = f.detail_left + column;
    const int visual_top =
        layout_timing(f, f.detail_left, left_right, groups_top) +
        ui.scaled(section_gap);
    layout_visual(f, f.detail_left, left_right, visual_top);
    layout_overlay(f, left_right + f.gap, f.detail_right, groups_top);
  } else {
    const int visual_top =
        layout_timing(f, f.detail_left, f.detail_right, groups_top) +
        ui.scaled(section_gap);
    layout_visual(f, f.detail_left, f.detail_right, visual_top);
    layout_overlay(f, f.detail_left, f.detail_right,
                   visual_top + heights.visual + ui.scaled(section_gap));
  }
  return list_top + min_list + ui.scaled(16) + groups_height;
}

void layout_preview(const MainLayout &f) {
  if (!f.preview)
    return;
  const DialogLayout &ui = f.ui;
  const int width = f.width - f.margin - f.preview_left;
  const int button_width = ui.scaled(180);
  ui.move(IDC_PREVIEW_LABEL, f.preview_left, f.margin,
          width - button_width - ui.scaled(8), ui.scaled(20));
  ui.move(IDC_POSITION_ON_WINDOW, f.preview_left + width - button_width,
          f.margin, button_width, f.row);
  const int top = f.margin + f.row + ui.scaled(10);
  ui.move(IDC_PREVIEW, f.preview_left, top, width,
          std::max(ui.scaled(120), f.status_top - f.margin - top));
}

void layout_status(const MainLayout &f) {
  const DialogLayout &ui = f.ui;
  const int text_top = f.status_top + ui.scaled(5);
  const int connection_width = ui.scaled(170);
  const int runtime_width = ui.scaled(250);
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
        return panel.style == layerc::core::PanelStyle::bar &&
               panel.cooldown_ids.size() > 1;
      });
  const MainLayout frame =
      make_main_layout(shell_.window, shell_.advanced_visible, legacy_bars);
  show_controls(shell_.window, advanced_controls, frame.advanced);
  show_controls(shell_.window, legacy_bar_controls, frame.legacy_bars);
  show_controls(shell_.window, preview_controls, frame.preview);
  layout_sidebar(frame);
  const int detail_bottom =
      layout_cooldowns(frame, layout_profile(frame));
  layout_preview(frame);
  layout_status(frame);
  shell_.status_strip_top = frame.status_top;
  // The smallest client height that keeps every row visible.
  shell_.min_client_height = detail_bottom + frame.margin +
                             (frame.height - frame.status_top);

  RECT list_client{};
  GetClientRect(shell_.profile_list, &list_client);
  ListView_SetColumnWidth(shell_.profile_list, 0,
                          std::max(1L, list_client.right - list_client.left));
  RedrawWindow(shell_.window, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

} // namespace layerc::editor
