#include "dialog_layout.hpp"
#include "editor_automation_internal.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_resource.h"
#include "test_support.hpp"

#include <optional>
#include <string_view>
#include <variant>

namespace layerc::test::editor_dialog {

void automation_preset_contracts(bool &passed) {
  const auto literal_is_true =
      [](const std::optional<layerc::core::AutomationExpression> &value) {
        if (!value.has_value())
          return false;
        const auto *literal = std::get_if<bool>(&value->literal.storage);
        return value->kind == layerc::core::ExpressionKind::literal &&
               literal != nullptr && *literal;
      };
  const auto literal_is_one_second =
      [](const std::optional<layerc::core::AutomationExpression> &value) {
        if (!value.has_value())
          return false;
        const auto *literal =
            std::get_if<layerc::core::DurationValue>(&value->literal.storage);
        return value->kind == layerc::core::ExpressionKind::literal &&
               literal != nullptr &&
               literal->value == layerc::core::Milliseconds(1'000);
      };

  const auto blank = layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::blank, "blank-id", "unused-cooldown",
      "unused-panel");
  passed &= expect(blank.id == "blank-id" && blank.name == "New automation" &&
                       blank.enabled && blank.rules.empty(),
                   "the blank automation preset should contain no rules");

  const auto input = layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::input_cooldown, "input-id",
      "dash-cooldown");
  passed &= expect(input.id == "input-id" && input.name == "Input cooldown" &&
                       input.enabled && input.rules.size() == 1,
                   "the input preset should create one enabled automation");
  if (input.rules.size() == 1) {
    const auto &rule = input.rules.front();
    passed &= expect(
        rule.id == "on-input" &&
            rule.event == layerc::core::AutomationEventKind::input_pressed &&
            rule.source_filter == "keyboard:18" && rule.actions.size() == 1,
        "the input preset should listen for the default keyboard input");
    if (rule.actions.size() == 1)
      passed &= expect(
          rule.actions.front().kind ==
                  layerc::core::AutomationActionKind::trigger_cooldown &&
              rule.actions.front().target == "dash-cooldown",
          "the input preset should trigger the selected cooldown");
  }

  const auto timer = layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::repeating_session_timer, "timer-id",
      "pulse-cooldown");
  passed &= expect(timer.id == "timer-id" &&
                       timer.name == "Repeating session timer" &&
                       timer.enabled && timer.rules.size() == 2,
                   "the timer preset should create start and tick rules");
  if (timer.rules.size() == 2) {
    const auto &start = timer.rules[0];
    const auto &tick = timer.rules[1];
    passed &= expect(
        start.id == "start-timer" &&
            start.event == layerc::core::AutomationEventKind::session_started &&
            start.actions.size() == 1 && tick.id == "on-timer" &&
            tick.event == layerc::core::AutomationEventKind::timer_fired &&
            tick.source_filter == "timer-id-timer" &&
            tick.actions.size() == 1,
        "the timer preset should connect session start to its timer event");
    if (start.actions.size() == 1)
      passed &= expect(
          start.actions.front().kind ==
                  layerc::core::AutomationActionKind::create_timer &&
              start.actions.front().target == "timer-id-timer" &&
              start.actions.front().repeating &&
              literal_is_one_second(start.actions.front().value),
          "the timer preset should create a repeating one-second timer");
    if (tick.actions.size() == 1)
      passed &= expect(
          tick.actions.front().kind ==
                  layerc::core::AutomationActionKind::trigger_cooldown &&
              tick.actions.front().target == "pulse-cooldown",
          "the timer preset should trigger the selected cooldown on each tick");
  }

  const auto overlay = layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::ready_overlay_pulse, "overlay-id",
      "ready-cooldown", "main-panel");
  passed &= expect(overlay.id == "overlay-id" &&
                       overlay.name == "Ready overlay pulse" &&
                       overlay.enabled && overlay.rules.size() == 1,
                   "the overlay preset should create one enabled automation");
  if (overlay.rules.size() == 1) {
    const auto &rule = overlay.rules.front();
    passed &= expect(
        rule.id == "on-ready" &&
            rule.event == layerc::core::AutomationEventKind::cooldown_ready &&
            rule.source_filter == "ready-cooldown" &&
            rule.actions.size() == 1,
        "the overlay preset should listen for the selected cooldown becoming ready");
    if (rule.actions.size() == 1) {
      const auto &action = rule.actions.front();
      passed &= expect(
          action.kind ==
                  layerc::core::AutomationActionKind::set_overlay_override &&
              action.target == "main-panel" &&
              action.name == "overlay-id-ready" &&
              action.property == "visible" && literal_is_true(action.value) &&
              literal_is_one_second(action.secondary),
          "the overlay preset should show the panel for one second");
    }
  }
}

void automation_dialog_contracts(const HINSTANCE instance, bool &passed) {
  const HWND automation_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_AUTOMATION), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      automation_dialog != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_LIST) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_PRESETS) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_VIEW) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_PALETTE) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_NODES) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_RULE_ID) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_ACTION_KIND) !=
              nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_ADD_NODE) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_APPLY_NODE) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_DELETE_NODE) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_VALIDATION) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_JSON) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_SIM_EVENT) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_SIM_STEP) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_SIM_PLAY) != nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_SIM_BREAKPOINT) !=
              nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_SIM_ASSERTIONS) !=
              nullptr &&
          GetDlgItem(automation_dialog, IDC_AUTOMATION_PROPERTIES) == nullptr,
      "Automation should provide Basic, Simulator, and Advanced JSON controls "
      "through one View selector without the removed graph properties pane");
  if (automation_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        automation_dialog,
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND enabled = GetDlgItem(automation_dialog, IDC_AUTOMATION_ENABLED);
    const HWND trace = GetDlgItem(automation_dialog, IDC_AUTOMATION_TRACE);
    const HWND repeating =
        GetDlgItem(automation_dialog, IDC_AUTOMATION_REPEATING);
    const auto view_style = GetWindowLongPtrW(
        GetDlgItem(automation_dialog, IDC_AUTOMATION_VIEW), GWL_STYLE);
    passed &= expect((view_style & CBS_OWNERDRAWFIXED) != 0,
                     "themed automation selectors should use the shared "
                     "owner-drawn selection palette");
    passed &= expect((GetWindowLongPtrW(enabled, GWL_STYLE) & BS_TYPEMASK) ==
                             BS_AUTOCHECKBOX &&
                         (GetWindowLongPtrW(trace, GWL_STYLE) & BS_TYPEMASK) ==
                             BS_AUTOCHECKBOX &&
                         (GetWindowLongPtrW(repeating, GWL_STYLE) &
                          BS_TYPEMASK) == BS_AUTOCHECKBOX,
                     "automation enablement and live trace must retain native "
                     "checkbox semantics");
    SendMessageW(enabled, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(enabled, BM_CLICK, 0, 0);
    SendMessageW(trace, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(trace, BM_CLICK, 0, 0);
    passed &=
        expect(SendMessageW(enabled, BM_GETCHECK, 0, 0) == BST_CHECKED &&
                   SendMessageW(trace, BM_GETCHECK, 0, 0) == BST_CHECKED,
               "themed automation checkboxes should toggle through BM_CLICK");
    const auto hierarchy_style = GetWindowLongPtrW(
        GetDlgItem(automation_dialog, IDC_AUTOMATION_NODES), GWL_STYLE);
    passed &= expect(
        (hierarchy_style & (LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS)) ==
                (LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS) &&
            (hierarchy_style & LBS_OWNERDRAWVARIABLE) == 0,
        "the Basic Rule to Actions hierarchy should retain native notification "
        "and strings with themed fixed-height rows");
    passed &= expect(
        (GetWindowLongPtrW(GetDlgItem(automation_dialog, IDC_AUTOMATION_VIEW),
                           GWL_STYLE) &
         CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST,
        "the Automation View selector should remain a native dropdown list");
    passed &= expect(
        (GetWindowLongPtrW(GetDlgItem(automation_dialog, IDC_AUTOMATION_JSON),
                           GWL_STYLE) &
         ES_MULTILINE) != 0,
        "the Advanced typed-AST editor should remain multiline");
    passed &= expect(
        LOWORD(SendMessageW(automation_dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "Close should remain the automation dialog default action");
    DestroyWindow(automation_dialog);
  }
}

} // namespace layerc::test::editor_dialog

using layerc::test::expect;

void automation_palette_contracts() {
  using layerc::core::AutomationActionKind;
  using layerc::core::AutomationEventKind;
  using layerc::editor::AutomationPaletteNode;
  const auto &palette = layerc::editor::automation_palette;
  expect(palette.size() == 6U &&
             std::wstring_view(palette[0].label) == L"Event: input pressed" &&
             std::wstring_view(palette[5].label) == L"Variable: Boolean",
         "the palette table should keep the combo item order");
  expect(layerc::editor::automation_palette_entry(-1) == nullptr &&
             layerc::editor::automation_palette_entry(6) == nullptr &&
             layerc::editor::automation_palette_entry(4) == &palette[4],
         "palette lookup should reject out-of-range combo indices");

  const auto rule = layerc::editor::make_palette_rule(
      AutomationPaletteNode::timer_rule, "rule-2", "cooldown");
  expect(rule.id == "rule-2" && rule.event == AutomationEventKind::timer_fired &&
             rule.source_filter == "pulse" && rule.actions.size() == 1U &&
             rule.actions[0].kind == AutomationActionKind::reset_cooldown &&
             rule.actions[0].target == "cooldown",
         "a timer rule node should reset the first cooldown on pulse");
  const auto timer = layerc::editor::make_palette_action(
      AutomationPaletteNode::timer_action, 2U, "cooldown", "panel");
  expect(timer.kind == AutomationActionKind::create_timer &&
             timer.target == "pulse-3" && timer.value.has_value(),
         "timer action nodes should number after the existing actions");
  const auto overlay = layerc::editor::make_palette_action(
      AutomationPaletteNode::overlay_action, 1U, "cooldown", "panel");
  expect(overlay.kind == AutomationActionKind::set_overlay_override &&
             overlay.target == "panel" && overlay.name == "visibility-2" &&
             overlay.property == "visible" && overlay.value.has_value() &&
             overlay.secondary.has_value(),
         "overlay action nodes should pulse the first panel's visibility");
  const auto flag = layerc::editor::make_palette_variable("flag-1");
  expect(flag.id == "flag-1" &&
             flag.type == layerc::core::AutomationValueType::boolean &&
             flag.default_value == layerc::core::AutomationValue(false),
         "flag variable nodes should default to false");
}
