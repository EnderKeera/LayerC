#include "appearance_contrast.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_layouts.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_layout_rules.hpp"
#include "editor_resource.h"
#include "editor_shortcuts.hpp"
#include "static_style.hpp"
#include "test_support.hpp"
#include "visual_runtime_status.hpp"

#include "layerc/platform/native_theme.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
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

// Options with its text in a `pixels`-tall font (a larger text size): every
// check box label and hint shows whole, wrapped rather than ellipsized when
// the work area is too narrow for one line, and no row overlaps the next.
// `pixels` 0 picks a font too large for any label to fit its work area.
void expect_options_text_fits(int pixels) {
  const HWND options = open_dialog(IDD_OPTIONS);
  if (options == nullptr) {
    expect(false, "Options should load");
    return;
  }
  const bool must_wrap = pixels == 0;
  if (must_wrap)
    pixels = std::clamp(
        static_cast<int>(layerc::editor::work_area_client_size(options).cx / 8),
        96, 1000);
  const std::string name = "Options at a " + std::to_string(pixels) + " px font";
  int wrapped = 0;
  const HFONT font = CreateFontW(-pixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
  layerc::editor::style_native_dialog_children(options, font);
  layerc::editor::layout_options_dialog(options);
  const UINT dpi = layerc::platform::native_window_dpi(options);
  const int offset = layerc::platform::native_check_box_label_offset(dpi);
  const int ring = layerc::platform::native_check_box_ring_margin(dpi);
  RECT client{};
  GetClientRect(options, &client);
  const SIZE work = layerc::editor::work_area_client_size(options);
  const int floor = layerc::editor::dpi_scaled(
      480, layerc::editor::window_base_dpi(options));
  expect(client.right <= std::max<LONG>(floor, work.cx),
         name + ": the dialog must not grow wider than its work area");
  for (const int id : {IDC_VISUAL_MASTER, IDC_CAPTURE_PRIVACY,
                       IDC_EDITOR_CAPTURE_PRIVACY, IDC_LAUNCHER_UPDATES}) {
    const HWND box = GetDlgItem(options, id);
    const RECT rect = child_rect(options, id);
    const int width = rect.right - rect.left;
    const bool wrap =
        (GetWindowLongPtrW(box, GWL_STYLE) & BS_MULTILINE) != 0;
    const SIZE line = layerc::editor::control_text_extent(box);
    if (wrap) {
      ++wrapped;
      const SIZE lines = layerc::editor::control_text_extent(
          box, std::max(1, width - offset - ring));
      expect(line.cx + offset + ring > width &&
                 rect.bottom - rect.top >= lines.cy,
             name + ": wrapped check box " + std::to_string(id) +
                 " must be as tall as its lines");
    } else {
      expect(line.cx + offset + ring <= width,
             name + ": check box " + std::to_string(id) +
                 " must show its whole label on one line");
    }
    expect(rect.right <= client.right,
           name + ": check box " + std::to_string(id) + " leaves the client");
  }
  for (const int id : {IDC_OPTIONS_VISUAL_HINT, IDC_OPTIONS_CAPTURE_HINT,
                       IDC_TEXT_SIZE_HINT, IDC_OPTIONS_UPDATES_HINT}) {
    const RECT rect = child_rect(options, id);
    const SIZE lines = layerc::editor::control_text_extent(
        GetDlgItem(options, id), rect.right - rect.left);
    expect(rect.bottom - rect.top >= lines.cy && rect.right <= client.right,
           name + ": hint " + std::to_string(id) + " must show every line");
  }
  int previous = 0;
  for (const int id :
       {IDC_VISUAL_MASTER, IDC_OPTIONS_VISUAL_HINT, IDC_CAPTURE_PRIVACY,
        IDC_EDITOR_CAPTURE_PRIVACY, IDC_OPTIONS_CAPTURE_HINT,
        IDC_TEXT_SIZE_LABEL, IDC_TEXT_SIZE_HINT, IDC_LAUNCHER_UPDATES,
        IDC_OPTIONS_UPDATES_HINT, IDOK}) {
    const RECT rect = child_rect(options, id);
    expect(rect.top >= previous,
           name + ": control " + std::to_string(id) +
               " overlaps the row above it");
    previous = rect.bottom;
  }
  expect(previous <= client.bottom,
         name + ": the buttons must stay inside the client");
  expect(!must_wrap || wrapped > 0,
         name + ": a label wider than the work area must wrap");
  DestroyWindow(options);
  DeleteObject(font);
}

} // namespace

void editor_dialog_layout_contracts() {
  if (const HWND options = open_dialog(IDD_OPTIONS); options != nullptr) {
    layerc::editor::layout_options_dialog(options);
    expect_control_height(options, "Options",
                          {IDC_VISUAL_MASTER, IDC_CAPTURE_PRIVACY,
                           IDC_EDITOR_CAPTURE_PRIVACY, IDC_LAUNCHER_UPDATES,
                           IDOK, IDCANCEL, IDC_OPTIONS_APPLY});
    expect_button_order(options, "Options",
                        {IDOK, IDCANCEL, IDC_OPTIONS_APPLY});
    expect_flat_frames(options, "Options");
    DestroyWindow(options);
  } else {
    expect(false, "Options should load");
  }
  // The default font, a 130% text size, a larger one, and one too large for
  // the work area to show a label on one line.
  for (const int pixels : {12, 16, 24, 0})
    expect_options_text_fits(pixels);

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
  constexpr int slot = main_slot_width(1280, margin, detail_left, gap);
  static_assert(slot == 322 &&
                    detail_left + slot * 3 + gap * 2 <= 1280 - margin &&
                    detail_left + slot * 3 + gap * 2 > 1280 - margin - 3,
                "the three settings slots fill the detail column to the "
                "margin");
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

  // GUI-02: when larger text makes stacked rows taller than the work area,
  // height clamping alone would hide the visual controls below the status.
  using layerc::editor::main_minimum_width;
  expect(main_minimum_width(880, 900, 1152, 1920, 980) == 880 &&
             main_minimum_width(1146, 1170, 1500, 1920, 980) == 1500 &&
             main_minimum_width(1146, 980, 1500, 1920, 980) == 1146 &&
             main_minimum_width(1146, 1170, 1500, 1280, 980) == 1280 &&
             main_minimum_width(1146, 900, 1500, 1000, 980) == 1000,
         "main minimum width keeps stacked rows only when their height fits, "
         "and never requires a width beyond the work area");

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

  // Review GUI-05: every swatch is described by its own color. The
  // descriptions take no selected color, so moving the selection (Graphite
  // to Charcoal) cannot change them.
  const std::vector<layerc::editor::ContrastTarget> disabled_text{
      {L"Window disabled text", RGB(0x80, 0x80, 0x80), 4.5, true}};
  const auto swatches =
      layerc::editor::palette_swatch_descriptions(disabled_text);
  const auto &palette = layerc::platform::color_palette;
  bool each_own = swatches.size() == palette.size();
  for (std::size_t index = 0; each_own && index < palette.size(); ++index) {
    const auto own = layerc::editor::picker_contrast_line(
        layerc::platform::native_color_ref(palette[index].value),
        disabled_text);
    each_own = own && swatches[index] == own->text;
  }
  expect(each_own, "each swatch describes the contrast of its own color");
  expect(swatches.size() > 1 && palette[0].name == L"Graphite" &&
             palette[1].name == L"Charcoal" && swatches[0] != swatches[1],
         "Graphite and Charcoal describe different contrasts");
  expect(std::ranges::all_of(
             layerc::editor::palette_swatch_descriptions({}),
             [](const std::wstring &text) { return text.empty(); }),
         "swatches without contrast targets have no description");

  // A status kind draws the glyph of its meaning; `none` draws nothing.
  using layerc::editor::StatusKind;
  using layerc::editor::status_glyph;
  expect(status_glyph(StatusKind::success) == StatusGlyph::yes &&
             status_glyph(StatusKind::failure) == StatusGlyph::no &&
             status_glyph(StatusKind::warning) == StatusGlyph::warning &&
             status_glyph(StatusKind::information) == StatusGlyph::accent_2 &&
             !status_glyph(StatusKind::none),
         "each status kind maps to its glyph");

  // LayerD's visual recognition phrases, read only at the adapter.
  using layerc::editor::visual_runtime_status_kind;
  const std::pair<std::wstring_view, StatusKind> visual_states[]{
      {L"WGC active", StatusKind::success},
      {L"Desktop Duplication fallback", StatusKind::warning},
      {L"inactive", StatusKind::information},
      {L"disabled", StatusKind::information},
      {L"connecting", StatusKind::information},
      {L"blocked (invalid sidecar)", StatusKind::failure},
      {L"isolated helper quarantined (use Runtime Diagnostics to retry)",
       StatusKind::failure},
      {L"template asset missing or corrupt", StatusKind::failure},
      {L"capture unavailable", StatusKind::failure},
  };
  for (const auto &[state, kind] : visual_states) {
    const std::wstring text = L"Visual recognition: " + std::wstring(state);
    expect(visual_runtime_status_kind(text) == kind &&
               visual_runtime_status_kind(text + L" - NVIDIA GeForce RTX") ==
                   kind,
           "each visual runtime state keeps its kind with a GPU suffix");
  }
  expect(visual_runtime_status_kind(
             L"Visual recognition: capture unavailable - active GPU") ==
                 StatusKind::failure &&
             visual_runtime_status_kind(L"Visual recognition: rebooting") ==
                 StatusKind::information &&
             visual_runtime_status_kind(L"WGC active") ==
                 StatusKind::information &&
             visual_runtime_status_kind(L"Visual: WGC active") ==
                 StatusKind::information,
         "a word in the GPU name never decides; unknown or unprefixed text "
         "is information");
  expect(visual_runtime_status_kind(L"Visual recognition: WGC active-not-really") ==
             StatusKind::information &&
             visual_runtime_status_kind(L"Visual recognition: Desktop Duplication fallback-new-state") ==
             StatusKind::information,
         "a known phrase prefix is not a recognized state");

  const HWND editor = open_dialog(IDD_EDITOR);
  if (editor == nullptr) {
    expect(false, "the editor resource should load");
    return;
  }
  // Text-size changes must grow a retained narrow window to the same
  // scaled width floor as interactive resizing, without shrinking its height.
  const int prior_scale = layerc::platform::native_text_scale();
  for (const int scale : {100, 130}) {
    layerc::platform::set_native_text_scale(scale);
    const SIZE narrow = layerc::editor::window_size_for_client(editor, 400, 400);
    SetWindowPos(editor, nullptr, 0, 0, narrow.cx, narrow.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const SIZE work = layerc::editor::work_area_client_size(editor);
    const LONG minimum = std::min<LONG>(
        layerc::editor::dpi_scaled(layerc::editor::main_client_min_width,
                                  layerc::editor::window_base_dpi(editor)),
        work.cx);
    layerc::editor::grow_client_to(editor, {minimum, 0});
    RECT client{};
    GetClientRect(editor, &client);
    expect(client.right >= minimum && client.bottom == 400,
           "a retained narrow editor must grow to its text-scaled width floor");
  }
  layerc::platform::set_native_text_scale(prior_scale);

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
  // The strip stores the glyph of the kind it is given, whatever the text
  // says, and keeps the text as the accessible name.
  const HWND strip_control = GetDlgItem(editor, IDC_CONNECTION_STATUS);
  for (const auto kind : {StatusKind::failure, StatusKind::success,
                          StatusKind::warning, StatusKind::information,
                          StatusKind::none}) {
    layerc::editor::set_strip_status(strip_control, kind, L"LayerD: connected");
    const auto strip = layerc::editor::static_style(strip_control);
    wchar_t name[32]{};
    GetDlgItemTextW(editor, IDC_CONNECTION_STATUS, name, 32);
    expect(strip && strip->align_right && strip->glyph == status_glyph(kind) &&
               std::wstring_view(name) == L"LayerD: connected",
           "a strip state keeps its text as the accessible name beside the "
           "glyph of its kind");
  }
  DestroyWindow(editor);
}
