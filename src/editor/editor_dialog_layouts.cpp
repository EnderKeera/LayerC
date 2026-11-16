#include "editor_dialog_layouts.hpp"

#include "dialog_layout.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>

#include <dwmapi.h>

namespace layerc::editor {

RECT invisible_frame_insets(const HWND window) {
  RECT outer{};
  RECT visible{};
  if (GetWindowRect(window, &outer) == FALSE ||
      FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                   &visible, sizeof(visible))))
    return {};
  return {std::max(0L, visible.left - outer.left),
          std::max(0L, visible.top - outer.top),
          std::max(0L, outer.right - visible.right),
          std::max(0L, outer.bottom - visible.bottom)};
}

namespace {

// The shared frame: margins, control height, gaps, and the line height of
// the dialog's body text.
struct DialogMetrics {
  explicit DialogMetrics(const HWND dialog, const int line_id)
      : ui(dialog, window_base_dpi(dialog)), margin(ui.scaled(12)),
        gap(ui.scaled(8)), group(ui.scaled(16)),
        height(ui.scaled(native_control_height)),
        line(static_cast<int>(ui.text_extent(line_id).cy)),
        status(std::max(line, ui.scaled(20))) {}
  DialogLayout ui;
  int margin{};
  int gap{};
  int group{};
  int height{};
  int line{};
  int status{};
};

// A hint or caption under the control it belongs to, wrapped to `width`.
int place_text(const DialogLayout &ui, const int id, const int x,
               const int top, const int width) {
  const int lines = static_cast<int>(ui.text_extent(id, width).cy);
  ui.move(id, x, top, width, lines);
  return lines;
}

// A check box as the painter draws it (DesignSystem.md, CheckBox): the box,
// its label `offset` after the left edge, and the focus ring's `ring` after
// the label's text.
struct CheckBoxMetrics {
  int offset{};
  int ring{};
  [[nodiscard]] int width(const DialogLayout &ui, const int id) const {
    return offset + static_cast<int>(ui.text_extent(id).cx) + ring;
  }
};

// A check box row `width` wide. A label that fits stays on one line in a
// `row`-tall control; a longer one wraps (BS_MULTILINE) and the control grows
// by its extra lines, never ellipsized. Returns the height taken.
int place_check_box(const DialogLayout &ui, const CheckBoxMetrics &check,
                    const int id, const int x, const int top, const int width,
                    const int row) {
  const HWND control = ui.control(id);
  if (control == nullptr)
    return row;
  const bool wrap = check.width(ui, id) > width;
  const auto style = GetWindowLongPtrW(control, GWL_STYLE);
  const auto wanted = wrap ? style | BS_MULTILINE
                           : style & ~static_cast<LONG_PTR>(BS_MULTILINE);
  if (wanted != style)
    SetWindowLongPtrW(control, GWL_STYLE, wanted);
  int height = row;
  if (wrap) {
    const int line = static_cast<int>(ui.text_extent(id).cy);
    const int lines = static_cast<int>(
        ui.text_extent(id, std::max(1, width - check.offset - check.ring)).cy);
    height = std::max(row, lines + std::max(0, row - line));
  }
  ui.move(id, x, top, width, height);
  return height;
}

} // namespace

void layout_options_dialog(const HWND dialog) {
  const DialogMetrics m(dialog, IDC_TEXT_SIZE_LABEL);
  const auto &ui = m.ui;
  const UINT dpi = layerc::platform::native_window_dpi(dialog);
  // Hints line up with their check box's label, where the painter puts it.
  const CheckBoxMetrics check{
      .offset = layerc::platform::native_check_box_label_offset(dpi),
      .ring = layerc::platform::native_check_box_ring_margin(dpi)};
  const int indent = check.offset;
  constexpr std::array check_boxes{IDC_VISUAL_MASTER, IDC_CAPTURE_PRIVACY,
                                   IDC_EDITOR_CAPTURE_PRIVACY,
                                   IDC_LAUNCHER_UPDATES};
  const int label =
      static_cast<int>(ui.text_extent(IDC_TEXT_SIZE_LABEL).cx) + m.gap;
  const int combo = ui.scaled(110);
  // The content is the design width, or wider when a row's measured text
  // needs it, but never wider than the work area shows: a check box label
  // still too long wraps under itself, and hints always wrap.
  int width = std::max({ui.scaled(480) - m.margin * 2, label + combo,
                        ui.button_row_width({IDOK, IDCANCEL, IDC_OPTIONS_APPLY})});
  for (const int id : check_boxes)
    width = std::max(width, check.width(ui, id));
  width = static_cast<int>(std::min<LONG>(
      width, std::max<LONG>(label + combo,
                            work_area_client_size(dialog).cx - m.margin * 2)));
  int top = m.margin;
  top += place_check_box(ui, check, IDC_VISUAL_MASTER, m.margin, top, width,
                         m.height);
  top += place_text(ui, IDC_OPTIONS_VISUAL_HINT, m.margin + indent, top,
                    width - indent) +
         m.gap;
  top += place_check_box(ui, check, IDC_CAPTURE_PRIVACY, m.margin, top, width,
                         m.height) +
         m.gap;
  top += place_check_box(ui, check, IDC_EDITOR_CAPTURE_PRIVACY, m.margin, top,
                         width, m.height);
  top += place_text(ui, IDC_OPTIONS_CAPTURE_HINT, m.margin + indent, top,
                    width - indent) +
         m.gap;
  ui.row_label(IDC_TEXT_SIZE_LABEL, m.margin, top, label, m.height);
  ui.combo(IDC_TEXT_SIZE, m.margin + label, top, combo, m.height);
  top += m.height;
  top += place_text(ui, IDC_TEXT_SIZE_HINT, m.margin + label, top,
                    width - label) +
         m.gap;
  top += place_check_box(ui, check, IDC_LAUNCHER_UPDATES, m.margin, top, width,
                         m.height);
  top += place_text(ui, IDC_OPTIONS_UPDATES_HINT, m.margin + indent, top,
                    width - indent) +
         m.group;
  ui.button_row({IDOK, IDCANCEL, IDC_OPTIONS_APPLY}, m.margin + width, top,
                m.height);
  ui.fit_client(m.margin * 2 + width, top + m.height + m.margin);
}

void layout_appearance_dialog(const HWND dialog, const bool advanced) {
  const DialogMetrics m(dialog, IDC_APPEARANCE_PREVIEW_LABEL);
  const auto &ui = m.ui;
  const int width = ui.scaled(600) - m.margin * 2;
  const int caption = m.line + ui.scaled(4);
  int top = m.margin;
  const int preset =
      static_cast<int>(ui.text_extent(IDC_APPEARANCE_PRESET_LABEL).cx) + m.gap;
  ui.row_label(IDC_APPEARANCE_PRESET_LABEL, m.margin, top, preset, m.height);
  ui.combo(IDC_THEME_PRESET, m.margin + preset, top, ui.scaled(240), m.height);
  const int toggle = ui.button_width(IDC_APPEARANCE_ADVANCED);
  ui.move(IDC_APPEARANCE_ADVANCED, m.margin + width - toggle, top, toggle,
          m.height);
  top += m.height + m.group;
  ui.move(IDC_APPEARANCE_PREVIEW_LABEL, m.margin, top, width, m.line);
  top += caption;
  const int preview = ui.scaled(140);
  ui.move(IDC_APPEARANCE_PREVIEW, m.margin, top, width, preview);
  top += preview + m.group;
  if (advanced) {
    const int list = ui.scaled(180);
    const int right = m.margin + list + m.group;
    const int right_width = m.margin + width - right;
    ui.move(IDC_APPEARANCE_ROLE_LABEL, m.margin, top, list, m.line);
    ui.move(IDC_APPEARANCE_OVERRIDE_LABEL, right, top, right_width, m.line);
    top += caption;
    // Every role shows without scrolling: rows times the item height,
    // inside the list's own frame as measured, not assumed.
    const HWND roles = ui.control(IDC_APPEARANCE_TREE);
    const auto count = std::max<LRESULT>(1, SendMessageW(roles, LB_GETCOUNT, 0, 0));
    const auto row =
        std::max<LRESULT>(1, SendMessageW(roles, LB_GETITEMHEIGHT, 0, 0));
    RECT outer{};
    RECT inner{};
    GetWindowRect(roles, &outer);
    GetClientRect(roles, &inner);
    const int frame = std::max(
        0, static_cast<int>((outer.bottom - outer.top) - inner.bottom));
    const int list_height = static_cast<int>(count * row) + frame;
    ui.move(IDC_APPEARANCE_TREE, m.margin, top, list, list_height);
    constexpr std::array fields{
        IDC_APPEARANCE_FILL,  IDC_APPEARANCE_TEXT,      IDC_APPEARANCE_BORDER,
        IDC_APPEARANCE_HOVER, IDC_APPEARANCE_SELECTION, IDC_APPEARANCE_DISABLED};
    const int column = (right_width - m.gap * 2) / 3;
    for (std::size_t index = 0; index < fields.size(); ++index)
      ui.move(fields[index],
              right + static_cast<int>(index % 3) * (column + m.gap),
              top + static_cast<int>(index / 3) * (m.height + m.gap), column,
              m.height);
    ui.move(IDC_APPEARANCE_RESET, right, top + (m.height + m.gap) * 2,
            ui.button_width(IDC_APPEARANCE_RESET), m.height);
    top += std::max(list_height, m.height * 3 + m.gap * 2) + m.gap;
  }
  ui.move(IDC_APPEARANCE_CONTRAST, m.margin, top, width, m.status);
  top += m.status + m.group;
  ui.move(IDC_APPEARANCE_RESET_ALL, m.margin, top,
          ui.button_width(IDC_APPEARANCE_RESET_ALL), m.height);
  ui.button_row({IDOK, IDCANCEL, IDC_APPEARANCE_APPLY}, m.margin + width, top,
                m.height);
  ui.fit_client(m.margin * 2 + width, top + m.height + m.margin);
}

void layout_color_picker_dialog(const HWND dialog) {
  const DialogMetrics m(dialog, IDC_COLOR_PALETTE_LABEL);
  const auto &ui = m.ui;
  struct Channel {
    int label;
    int edit;
    int width;
  };
  constexpr std::array channels{
      Channel{IDC_COLOR_RED_LABEL, IDC_COLOR_RED, 48},
      Channel{IDC_COLOR_GREEN_LABEL, IDC_COLOR_GREEN, 48},
      Channel{IDC_COLOR_BLUE_LABEL, IDC_COLOR_BLUE, 48},
      Channel{IDC_COLOR_HEX_LABEL, IDC_COLOR_HEX, 80}};
  const auto label_width = [&](const int id) {
    return static_cast<int>(ui.text_extent(id).cx) + ui.scaled(6);
  };
  int custom = -m.group;
  for (const auto &channel : channels)
    custom += label_width(channel.label) + ui.scaled(channel.width) + m.group;
  constexpr int columns = 8;
  const int width = std::max(ui.scaled(376), custom);
  const int caption = m.line + ui.scaled(4);
  int top = m.margin;
  ui.move(IDC_COLOR_PALETTE_LABEL, m.margin, top, width, m.line);
  top += caption;
  const int swatch = (width - m.gap * (columns - 1)) / columns;
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id) {
    const int index = id - IDC_COLOR_SWATCH_FIRST;
    ui.move(id, m.margin + (index % columns) * (swatch + m.gap),
            top + (index / columns) * (m.height + m.gap), swatch, m.height);
  }
  const int rows = (IDC_COLOR_SWATCH_LAST - IDC_COLOR_SWATCH_FIRST) / columns + 1;
  top += rows * m.height + (rows - 1) * m.gap + m.group;
  ui.move(IDC_COLOR_CUSTOM_LABEL, m.margin, top, width, m.line);
  top += caption;
  int x = m.margin;
  for (const auto &channel : channels) {
    const int label = label_width(channel.label);
    ui.row_label(channel.label, x, top, label, m.height);
    ui.move(channel.edit, x + label, top, ui.scaled(channel.width), m.height);
    x += label + ui.scaled(channel.width) + m.group;
  }
  top += m.height;
  top += place_text(ui, IDC_COLOR_HINT, m.margin, top, width) + m.group;
  const int preview = ui.scaled(40);
  const int preview_label =
      static_cast<int>(ui.text_extent(IDC_COLOR_PREVIEW_LABEL).cx) + m.gap;
  ui.row_label(IDC_COLOR_PREVIEW_LABEL, m.margin, top, preview_label, preview);
  ui.move(IDC_COLOR_PREVIEW, m.margin + preview_label, top, ui.scaled(120),
          preview);
  top += preview + m.gap;
  ui.move(IDC_COLOR_CONTRAST, m.margin, top, width, m.status);
  top += m.status + m.group;
  ui.button_row({IDOK, IDCANCEL}, m.margin + width, top, m.height);
  ui.fit_client(m.margin * 2 + width, top + m.height + m.margin);
}

void layout_shortcuts_dialog(const HWND dialog, const std::size_t rows) {
  const DialogMetrics m(dialog, IDOK);
  const auto &ui = m.ui;
  int keys = 0;
  int actions = 0;
  for (std::size_t row = 0; row < rows; ++row) {
    const int id = IDC_SHORTCUT_FIRST + static_cast<int>(row) * 2;
    keys = std::max(keys, static_cast<int>(ui.text_extent(id).cx));
    actions = std::max(actions, static_cast<int>(ui.text_extent(id + 1).cx));
  }
  // One left-aligned key column as wide as its longest entry.
  const int column_gap = ui.scaled(24);
  const int pitch = m.line + m.gap;
  int top = m.margin;
  for (std::size_t row = 0; row < rows; ++row) {
    const int id = IDC_SHORTCUT_FIRST + static_cast<int>(row) * 2;
    ui.row_label(id, m.margin, top, keys, pitch);
    ui.row_label(id + 1, m.margin + keys + column_gap, top, actions, pitch);
    top += pitch;
  }
  top += m.group;
  const int width = std::max(keys + column_gap + actions, ui.button_width(IDOK));
  ui.button_row({IDOK}, m.margin + width, top, m.height);
  ui.fit_client(m.margin * 2 + width, top + m.height + m.margin);
}

} // namespace layerc::editor
