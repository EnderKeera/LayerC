#include "appearance_contrast.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_layouts.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_layout_rules.hpp"
#include "editor_resource.h"
#include "editor_shortcuts.hpp"
#include "static_style.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// The code-laid-out dialogs (DesignSystem.md, Dialogs): one control height,
// flat frames, OK · Cancel · Apply at the bottom right, and the Keyboard
// shortcuts table generated from the accelerators. The main window
// (DesignSystem.md, Layout): three settings slots, the preview corner over
// slot 3, reading-order tab stops, and the lines that explain a state.
namespace {

using layerc::test::expect;

RECT child_rect(const HWND dialog, const int id) {
  RECT rect{};
  if (const HWND control = GetDlgItem(dialog, id); control != nullptr) {
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT *>(&rect), 2);
  }
  return rect;
}

HWND open_dialog(const int resource) {
  const HWND dialog = CreateDialogParamW(
      GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource), nullptr,
      &layerc::test::editor_dialog::test_dialog_proc, 0);
  if (dialog != nullptr)
    layerc::editor::style_native_dialog_children(
        dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
  return dialog;
}

void expect_control_height(const HWND dialog, const std::string_view name,
                           const std::initializer_list<int> ids) {
  const int height = layerc::editor::dpi_scaled(
      layerc::editor::native_control_height,
      layerc::editor::window_base_dpi(dialog));
  for (const int id : ids) {
    const RECT rect = child_rect(dialog, id);
    expect(rect.bottom - rect.top == height,
           std::string(name) + ": control " + std::to_string(id) + " is " +
               std::to_string(rect.bottom - rect.top) + " px tall, not " +
               std::to_string(height));
  }
}

// Buttons in the given order, left to right on one row, the last one a
// margin from the client's right edge.
void expect_button_order(const HWND dialog, const std::string_view name,
                         const std::initializer_list<int> ids) {
  RECT client{};
  GetClientRect(dialog, &client);
  const int margin = layerc::editor::dpi_scaled(
      12, layerc::editor::window_base_dpi(dialog));
  const RECT *previous{};
  RECT last{};
  std::vector<RECT> rects;
  for (const int id : ids)
    rects.push_back(child_rect(dialog, id));
  for (const auto &rect : rects) {
    if (previous != nullptr)
      expect(rect.left > previous->right && rect.top == previous->top,
             std::string(name) + ": buttons must run OK, Cancel, Apply on "
                                 "one row");
    previous = &rect;
    last = rect;
  }
  expect(client.right - last.right == margin,
         std::string(name) + ": the last button must sit a margin from the "
                             "right edge");
}

// No bordered control keeps a sunken 3D edge after styling.
void expect_flat_frames(const HWND dialog, const std::string_view name) {
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    const auto ex_style = GetWindowLongPtrW(child, GWL_EXSTYLE);
    wchar_t class_name[32]{};
    GetClassNameW(child, class_name, 32);
    if (std::wstring_view(class_name) == L"Static")
      expect((ex_style & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE)) == 0,
             std::string(name) + ": static " +
                 std::to_string(GetDlgCtrlID(child)) +
                 " keeps a 3D edge instead of a flat frame");
  }
}

} // namespace

void editor_dialog_layout_contracts() {
  if (const HWND options = open_dialog(IDD_OPTIONS); options != nullptr) {
    layerc::editor::layout_options_dialog(options);
    expect_control_height(options, "Options",
                          {IDC_VISUAL_MASTER, IDC_CAPTURE_PRIVACY,
                           IDC_EDITOR_CAPTURE_PRIVACY, IDOK, IDCANCEL,
                           IDC_OPTIONS_APPLY});
    expect_button_order(options, "Options",
                        {IDOK, IDCANCEL, IDC_OPTIONS_APPLY});
    expect_flat_frames(options, "Options");
    DestroyWindow(options);
  } else {
    expect(false, "Options should load");
  }

  for (const bool advanced : {false, true}) {
    const HWND appearance = open_dialog(IDD_APPEARANCE);
    if (appearance == nullptr) {
      expect(false, "Appearance should load");
      break;
    }
    const HWND roles = GetDlgItem(appearance, IDC_APPEARANCE_TREE);
    constexpr int role_count = 10;
    for (int role = 0; role < role_count; ++role)
      SendMessageW(roles, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"Role"));
    layerc::editor::layout_appearance_dialog(appearance, advanced);
    if (advanced) {
      RECT client{};
      GetClientRect(roles, &client);
      const auto row = SendMessageW(roles, LB_GETITEMHEIGHT, 0, 0);
      expect(client.bottom >= role_count * row,
             "Appearance: every control role must show without scrolling");
    }
    expect_control_height(appearance, "Appearance",
                          {IDC_APPEARANCE_ADVANCED, IDC_APPEARANCE_RESET_ALL,
                           IDOK, IDCANCEL, IDC_APPEARANCE_APPLY});
    if (advanced)
      expect_control_height(
          appearance, "Appearance",
          {IDC_APPEARANCE_FILL, IDC_APPEARANCE_TEXT, IDC_APPEARANCE_BORDER,
           IDC_APPEARANCE_HOVER, IDC_APPEARANCE_SELECTION,
           IDC_APPEARANCE_DISABLED, IDC_APPEARANCE_RESET});
    expect_button_order(appearance, "Appearance",
                        {IDOK, IDCANCEL, IDC_APPEARANCE_APPLY});
    const RECT reset_all = child_rect(appearance, IDC_APPEARANCE_RESET_ALL);
    const RECT ok = child_rect(appearance, IDOK);
    const RECT contrast = child_rect(appearance, IDC_APPEARANCE_CONTRAST);
    expect(reset_all.top == ok.top && reset_all.right < ok.left,
           "Appearance: Reset all must sit left of OK on the button row");
    expect(contrast.bottom <= ok.top && contrast.bottom > contrast.top,
           "Appearance: the contrast line must keep its row above the "
           "buttons");
    expect_flat_frames(appearance, "Appearance");
    DestroyWindow(appearance);
  }

  if (const HWND picker = open_dialog(IDD_COLOR_PICKER); picker != nullptr) {
    layerc::editor::layout_color_picker_dialog(picker);
    expect_control_height(picker, "Choose color",
                          {IDC_COLOR_SWATCH_FIRST, IDC_COLOR_SWATCH_LAST,
                           IDC_COLOR_RED, IDC_COLOR_GREEN, IDC_COLOR_BLUE,
                           IDC_COLOR_HEX, IDOK, IDCANCEL});
    expect_button_order(picker, "Choose color", {IDOK, IDCANCEL});
    expect_flat_frames(picker, "Choose color");
    DestroyWindow(picker);
  } else {
    expect(false, "Choose color should load");
  }
}

void editor_shortcut_contracts() {
  using layerc::editor::accelerator_text;
  const auto accelerators =
      layerc::editor::editor_accelerators(GetModuleHandleW(nullptr));
  const auto table = layerc::editor::editor_shortcuts();
  expect(!accelerators.empty(), "the editor accelerator table should load");
  // Every accelerator is described by a row that names its keys.
  for (const auto &accelerator : accelerators) {
    const auto keys = accelerator_text(accelerator);
    const bool described = std::ranges::any_of(table, [&](const auto &entry) {
      return std::ranges::find(entry.commands, accelerator.cmd) !=
                 entry.commands.end() &&
             entry.keys.find(keys) != std::wstring_view::npos;
    });
    expect(described, "the shortcuts table must describe accelerator " +
                          std::to_string(accelerator.cmd));
  }
  expect(!table.empty() && table.back().keys == L"F1",
         "F1 Keyboard shortcuts must be the table's last row");
  expect(accelerator_text({FVIRTKEY | FCONTROL | FSHIFT, 'S', 0}) ==
                 L"Ctrl+Shift+S" &&
             accelerator_text({FVIRTKEY | FCONTROL, VK_OEM_COMMA, 0}) ==
                 L"Ctrl+," &&
             accelerator_text({FVIRTKEY, VK_F1, 0}) == L"F1",
         "accelerator keys must read as the menus write them");

  // Documented accelerators produce the table itself; an undocumented one
  // joins it by its menu words, still ahead of F1.
  const auto rows = layerc::editor::shortcut_rows(
      accelerators, [](int) { return std::wstring(); });
  expect(rows.size() == table.size() && rows.back().keys == L"F1",
         "documented accelerators must not add rows");
  auto extended = accelerators;
  constexpr WORD unlisted = 0x7FF0;
  extended.push_back({FVIRTKEY | FCONTROL, 'Q', unlisted});
  const auto more = layerc::editor::shortcut_rows(extended, [](const int id) {
    return id == unlisted ? std::wstring(L"Quit") : std::wstring();
  });
  expect(more.size() == table.size() + 1 && more.back().keys == L"F1" &&
             more[more.size() - 2].keys == L"Ctrl+Q" &&
             more[more.size() - 2].action == L"Quit",
         "an accelerator missing from the table must still be listed");

  // One key column: every key cell starts at the same x, every action at
  // the same x to its right.
  const HWND dialog = open_dialog(IDD_SHORTCUTS);
  if (dialog == nullptr) {
    expect(false, "Keyboard shortcuts should load");
    return;
  }
  layerc::editor::create_shortcut_cells(dialog, rows);
  layerc::editor::layout_shortcuts_dialog(dialog, rows.size());
  const RECT first_keys = child_rect(dialog, IDC_SHORTCUT_FIRST);
  const RECT first_action = child_rect(dialog, IDC_SHORTCUT_FIRST + 1);
  bool aligned = first_action.left > first_keys.right;
  for (std::size_t row = 0; row < rows.size(); ++row) {
    const int id = IDC_SHORTCUT_FIRST + static_cast<int>(row) * 2;
    const RECT keys = child_rect(dialog, id);
    const RECT action = child_rect(dialog, id + 1);
    aligned = aligned && keys.left == first_keys.left &&
              action.left == first_action.left && keys.top == action.top;
  }
  expect(aligned, "Keyboard shortcuts must keep one aligned key column");
  wchar_t last[16]{};
  GetDlgItemTextW(dialog,
                  IDC_SHORTCUT_FIRST + static_cast<int>(rows.size() - 1) * 2,
                  last, 16);
  expect(std::wstring_view(last) == L"F1",
         "the dialog's last row must be F1");
  expect_control_height(dialog, "Keyboard shortcuts", {IDOK});
  expect_button_order(dialog, "Keyboard shortcuts", {IDOK});
  DestroyWindow(dialog);
}

void editor_main_layout_contracts() {
  using layerc::editor::main_groups_top;
  using layerc::editor::main_layout_wide;
  using layerc::editor::main_slot_width;
  using layerc::platform::StatusGlyph;

  // Three equal slots from the detail column to the right margin.
  constexpr int margin = 12;
  constexpr int gap = 16;
  constexpr int detail_left = margin + 240 + gap;
  constexpr int preview_from = 1100;
  constexpr int slot_min = 260;
  const int slot = main_slot_width(1280, margin, detail_left, gap);
  expect(slot == 322 &&
             detail_left + slot * 3 + gap * 2 <= 1280 - margin &&
             detail_left + slot * 3 + gap * 2 > 1280 - margin - 3,
         "the three settings slots fill the detail column to the margin");
  expect(main_layout_wide(1280, slot, preview_from, slot_min),
         "the default window shows three slots and the preview corner");
  expect(!main_layout_wide(preview_from - 1, slot_min, preview_from,
                           slot_min) &&
             main_layout_wide(preview_from, slot_min, preview_from,
                              slot_min) &&
             !main_layout_wide(preview_from, slot_min - 1, preview_from,
                               slot_min),
         "a narrow window or a slot below its minimum stacks the slots");
  for (const int dpi : {96, 144, 192}) {
    const auto scaled = [dpi](const int value) { return value * dpi / 96; };
    expect(main_layout_wide(
               scaled(1280),
               main_slot_width(scaled(1280), scaled(margin),
                               scaled(detail_left), scaled(gap)),
               scaled(preview_from), scaled(slot_min)),
           "the three-slot decision is independent of DPI scaling");
  }
  // The settings row is anchored to the bottom; the list takes the rest
  // with no cap and only pushes the row down at its minimum.
  expect(main_groups_top(200, 84, gap, 700, 250) == 450 &&
             main_groups_top(200, 84, gap, 1400, 250) == 1150,
         "the cooldown list grows with the window instead of stopping at a "
         "cap");
  expect(main_groups_top(200, 84, gap, 500, 250) == 300,
         "the settings row moves down once the list is at its minimum");

  // Overlay colors only warn, in the Writing rules' words.
  using layerc::editor::OverlayColorField;
  using layerc::editor::OverlayColors;
  const OverlayColors readable{.fill = RGB(0, 90, 160),
                               .background = RGB(16, 16, 16),
                               .border = RGB(200, 200, 200),
                               .text = RGB(255, 255, 255)};
  expect(layerc::editor::overlay_contrast_text(readable).empty(),
         "readable overlay colors show no contrast line");
  OverlayColors faint = readable;
  faint.text = RGB(40, 100, 170);
  faint.border = RGB(30, 30, 30);
  const auto warning = layerc::editor::overlay_contrast_text(faint);
  expect(warning.starts_with(L"Low contrast. Text is ") &&
             warning.find(L" on Fill and ") != std::wstring::npos &&
             warning.find(L" on Background; aim for 4.5:1.") !=
                 std::wstring::npos &&
             warning.find(L" Border is ") != std::wstring::npos &&
             warning.ends_with(L" on Background; aim for 3:1."),
         "the overlay contrast line names each pair and its aim");
  faint.show_text = false;
  const auto border_only = layerc::editor::overlay_contrast_text(faint);
  expect(border_only.starts_with(L"Low contrast. Border is ") &&
             border_only.find(L"Text is") == std::wstring::npos,
         "hidden bar text is not measured");
  for (const auto field :
       {OverlayColorField::fill, OverlayColorField::background,
        OverlayColorField::border, OverlayColorField::text})
    for (const auto &target :
         layerc::editor::overlay_contrast_targets(readable, field))
      expect(!target.required,
             "Choose color never refuses an overlay bar color");
  const auto text_targets = layerc::editor::overlay_contrast_targets(
      readable, OverlayColorField::text);
  const auto border_targets = layerc::editor::overlay_contrast_targets(
      readable, OverlayColorField::border);
  expect(text_targets.size() == 2 && text_targets[0].name == L"Fill" &&
             text_targets[1].name == L"Background" &&
             text_targets[0].minimum == 4.5 && border_targets.size() == 1 &&
             border_targets[0].name == L"Background" &&
             border_targets[0].minimum == 3.0,
         "bar text is measured on Fill and Background, the border on "
         "Background");
  const auto line = layerc::editor::picker_contrast_line(
      RGB(40, 100, 170), text_targets);
  expect(line && line->below_minimum &&
             line->text.ends_with(L"; aim for 4.5:1."),
         "Choose color states the aim for a bar color, not a requirement");

  // Every status strip state carries the glyph of its meaning.
  using layerc::editor::status_strip_glyph;
  expect(status_strip_glyph(L"LayerD: not running") == StatusGlyph::no &&
             status_strip_glyph(L"LayerD: connecting") ==
                 StatusGlyph::accent_2 &&
             status_strip_glyph(L"LayerD: connected") == StatusGlyph::yes,
         "the LayerD state maps to no, accent_2 and yes");
  expect(status_strip_glyph(L"Visual: WGC active") == StatusGlyph::yes &&
             status_strip_glyph(L"Visual: Desktop Duplication fallback") ==
                 StatusGlyph::warning &&
             status_strip_glyph(L"Visual: inactive") ==
                 StatusGlyph::accent_2 &&
             status_strip_glyph(L"Visual: disabled") ==
                 StatusGlyph::accent_2 &&
             status_strip_glyph(L"Visual: blocked (invalid sidecar)") ==
                 StatusGlyph::no &&
             status_strip_glyph(L"Visual: isolated helper quarantined") ==
                 StatusGlyph::no &&
             status_strip_glyph(
                 L"Visual: template asset missing or corrupt") ==
                 StatusGlyph::no &&
             status_strip_glyph(L"Visual: capture unavailable") ==
                 StatusGlyph::no,
         "each visual runtime state maps to its glyph");

  const HWND editor = open_dialog(IDD_EDITOR);
  if (editor == nullptr) {
    expect(false, "the editor resource should load");
    return;
  }
  // Tab order is reading order: fields, list, preview corner, slots 1 to 3.
  std::vector<int> order;
  for (HWND child = GetWindow(editor, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT))
    order.push_back(GetDlgCtrlID(child));
  const auto position = [&order](const int id) {
    return std::ranges::find(order, id) - order.begin();
  };
  bool reading_order = true;
  int previous = 0;
  for (const int id :
       {IDC_PROFILE_NAME, IDC_APPLICATION_PATH, IDC_ADVANCED,
        IDC_COOLDOWN_LIST, IDC_ADD_COOLDOWN, IDC_REMOVE_COOLDOWN,
        IDC_POSITION_ON_WINDOW, IDC_PREVIEW, IDC_COOLDOWN_NAME,
        IDC_COOLDOWN_DURATION, IDC_CAPTURE_BINDING, IDC_VISUAL_ENABLED,
        IDC_CALIBRATE_VISUAL, IDC_BAR_WIDTH, IDC_BAR_FILL, IDC_SHOW_TEXT,
        IDC_DIRECT_DRAG}) {
    reading_order = reading_order && position(id) < std::ssize(order) &&
                    (previous == 0 || position(previous) < position(id));
    previous = id;
  }
  expect(reading_order,
         "tab order must run fields, list, preview, then slots 1 to 3");
  // A reason line follows the buttons it explains, so a screen reader meets
  // it next; state lines draw their own glyph.
  expect(position(IDC_COOLDOWN_REMOVE_REASON) ==
                 position(IDC_REMOVE_COOLDOWN) + 1 &&
             position(IDC_ITEM_REASON) == position(IDC_ITEM_DOWN) + 1,
         "a disabled button's reason line must follow the button");
  for (const int id : {IDC_OVERLAY_CONTRAST, IDC_CONNECTION_STATUS,
                       IDC_VISUAL_RUNTIME_STATUS})
    expect((GetWindowLongPtrW(GetDlgItem(editor, id), GWL_STYLE) &
            SS_TYPEMASK) == SS_OWNERDRAW,
           "status line " + std::to_string(id) + " must draw its own glyph");
  for (const int id : {IDC_COOLDOWN_REMOVE_REASON, IDC_ITEM_REASON,
                       IDC_POSITION_STATUS, IDC_OVERLAY_CONTRAST})
    expect((GetWindowLongPtrW(GetDlgItem(editor, id), GWL_STYLE) &
            WS_TABSTOP) == 0,
           "explanatory line " + std::to_string(id) +
               " must not be a tab stop");
  // The list and the preview end on one line, which a list box that snaps to
  // whole rows cannot do.
  for (const int id : {IDC_COOLDOWN_LIST, IDC_PANEL_ITEMS})
    expect((GetWindowLongPtrW(GetDlgItem(editor, id), GWL_STYLE) &
            LBS_NOINTEGRALHEIGHT) != 0,
           "list " + std::to_string(id) + " must take the height it is given");
  layerc::editor::set_strip_status(
      GetDlgItem(editor, IDC_CONNECTION_STATUS), L"LayerD: connected");
  const auto strip =
      layerc::editor::static_style(GetDlgItem(editor, IDC_CONNECTION_STATUS));
  wchar_t name[32]{};
  GetDlgItemTextW(editor, IDC_CONNECTION_STATUS, name, 32);
  expect(strip && strip->align_right && strip->glyph == StatusGlyph::yes &&
             std::wstring_view(name) == L"LayerD: connected",
         "a strip state keeps its text as the accessible name beside the "
         "glyph");
  DestroyWindow(editor);
}
