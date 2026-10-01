#include "appearance_draft.hpp"
#include "dialog_layout.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <commctrl.h>
#include <windows.h>

namespace {

int apply_commands{};

INT_PTR CALLBACK test_dialog_proc(const HWND window, const UINT message,
                                  const WPARAM wparam, LPARAM) {
  if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
    ++apply_commands;
    return TRUE;
  }
  if (message == WM_CLOSE) {
    DestroyWindow(window);
    return TRUE;
  }
  return FALSE;
}

using layerc::test::expect;

bool menu_has_command(const HMENU menu, const UINT command) {
  return menu != nullptr &&
         GetMenuState(menu, command, MF_BYCOMMAND) != static_cast<UINT>(-1);
}

std::wstring menu_caption(const HMENU menu, const int position) {
  wchar_t text[32]{};
  const int length =
      GetMenuStringW(menu, static_cast<UINT>(position), text,
                     static_cast<int>(sizeof(text) / sizeof(text[0])),
                     MF_BYPOSITION);
  return length > 0 ? std::wstring(text, static_cast<std::size_t>(length))
                    : std::wstring{};
}

RECT child_rect(const HWND dialog, const int id) {
  RECT result{};
  const HWND child = GetDlgItem(dialog, id);
  if (child != nullptr && GetWindowRect(child, &result) != FALSE)
    MapWindowPoints(HWND_DESKTOP, dialog,
                    reinterpret_cast<POINT *>(&result), 2);
  return result;
}

} // namespace

void editor_dialog_contracts() {
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_OPTIONS), nullptr, &test_dialog_proc, 0);
  if (dialog == nullptr) {
    (void)expect(false, "the native Options dialog should load");
    return;
  }

  layerc::editor::style_native_dialog_children(
      dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
  const HWND apply = GetDlgItem(dialog, IDOK);
  const HWND checkbox = GetDlgItem(dialog, IDC_CAPTURE_PRIVACY);
  const HWND visual_checkbox = GetDlgItem(dialog, IDC_VISUAL_MASTER);
  const auto apply_type = GetWindowLongPtrW(apply, GWL_STYLE) & BS_TYPEMASK;
  const auto checkbox_type =
      GetWindowLongPtrW(checkbox, GWL_STYLE) & BS_TYPEMASK;
  const auto visual_checkbox_type =
      GetWindowLongPtrW(visual_checkbox, GWL_STYLE) & BS_TYPEMASK;
  bool passed = true;
  passed &=
      expect(apply_type == BS_OWNERDRAW &&
                 LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDOK &&
                 GetDlgItem(dialog, IDC_OPTIONS_APPLY) != nullptr &&
                 GetDlgItem(dialog, IDC_EDITOR_CAPTURE_PRIVACY) != nullptr,
             "the themed OK button must retain dialog-manager default semantics");
  passed &=
      expect(checkbox_type == BS_AUTOCHECKBOX,
             "the capture checkbox must retain native auto-toggle semantics");
  passed &= expect(
      visual_checkbox_type == BS_AUTOCHECKBOX,
      "the visual master checkbox must retain native auto-toggle semantics");

  SendMessageW(checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(checkbox, BM_CLICK, 0, 0);
  passed &= expect(SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED,
                   "a themed native checkbox should toggle through BM_CLICK");
  SendMessageW(visual_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(visual_checkbox, BM_CLICK, 0, 0);
  passed &= expect(
      SendMessageW(visual_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED,
      "the themed visual master checkbox should toggle through BM_CLICK");

  const LRESULT default_id = SendMessageW(dialog, DM_GETDEFID, 0, 0);
  passed &=
      expect(HIWORD(default_id) == DC_HASDEFID &&
                 LOWORD(default_id) == IDOK,
             "the Options dialog should advertise OK as its default action");
  MSG enter{};
  enter.hwnd = checkbox;
  enter.message = WM_KEYDOWN;
  enter.wParam = VK_RETURN;
  enter.lParam = 1;
  (void)IsDialogMessageW(dialog, &enter);
  passed &= expect(apply_commands == 1,
                   "Enter should dispatch the modal dialog's default action");

  layerc::core::GameProfile original;
  original.id = "profile";
  original.name = "Original";
  const std::optional<layerc::core::VisualProfile> no_visual;
  const std::optional<layerc::core::AutomationProfile> no_automation;
  layerc::editor::EditorProfileHistory history;
  history.reset(original, no_visual, no_automation);
  passed &=
      expect(!history.is_dirty(original, no_visual, no_automation, true),
             "a persisted profile matching its saved snapshot should be clean");
  passed &=
      expect(history.is_dirty(original, no_visual, no_automation, false),
             "a new draft must remain dirty at its initial history snapshot");
  auto edited = original;
  edited.name = "First text";
  passed &= expect(history.record(edited, no_visual, no_automation, true),
                   "the first text edit should create a history transaction");
  edited.name = "Coalesced text";
  passed &= expect(history.record(edited, no_visual, no_automation, true),
                   "a later text edit should update the coalesced transaction");
  auto undone = history.undo(edited, no_visual, no_automation);
  passed &= expect(undone.has_value() && undone->profile.name == "Original" &&
                       history.can_redo(),
                   "coalesced text edits should undo as one transaction");
  auto redone = undone.has_value()
                    ? history.redo(undone->profile, undone->visual_profile,
                                   undone->automation_profile)
                    : std::nullopt;
  passed &=
      expect(redone.has_value() && redone->profile.name == "Coalesced text",
             "a coalesced editor transaction should redo exactly");
  if (undone.has_value() && redone.has_value()) {
    passed &=
        expect(history.is_dirty(undone->profile, undone->visual_profile,
                                undone->automation_profile, false) &&
                   history.is_dirty(redone->profile, redone->visual_profile,
                                    redone->automation_profile, false),
               "draft undo and redo states must remain dirty until first save");
  }

  if (redone.has_value()) {
    auto immediate = redone->profile;
    immediate.enabled = false;
    history.end_text_coalescing();
    passed &= expect(history.record(immediate, redone->visual_profile,
                                    redone->automation_profile, false),
                     "an immediate edit should create a separate transaction");
    history.rebase(immediate, redone->visual_profile,
                   redone->automation_profile);
    passed &= expect(history.is_saved(immediate, redone->visual_profile,
                                      redone->automation_profile),
                     "rebase should mark the successfully persisted profile");
    const auto undo_immediate = history.undo(immediate, redone->visual_profile,
                                             redone->automation_profile);
    passed &=
        expect(undo_immediate.has_value() && undo_immediate->profile.enabled,
               "autosave rebase should preserve earlier undo history");
    if (undo_immediate.has_value()) {
      passed &=
          expect(!history.is_saved(undo_immediate->profile,
                                   undo_immediate->visual_profile,
                                   undo_immediate->automation_profile),
                 "undo away from the persisted state should remain dirty");
      const auto redo_immediate =
          history.redo(undo_immediate->profile, undo_immediate->visual_profile,
                       undo_immediate->automation_profile);
      passed &=
          expect(redo_immediate.has_value() &&
                     history.is_saved(redo_immediate->profile,
                                      redo_immediate->visual_profile,
                                      redo_immediate->automation_profile),
                 "redo back to the persisted state should clear dirty state");
    }
  }

  history.reset(original, no_visual, no_automation);
  layerc::core::VisualProfile assigned_visual;
  assigned_visual.profile_id = original.id;
  layerc::core::PixelRule ready_rule;
  ready_rule.id = "ready";
  ready_rule.name = "Ready";
  assigned_visual.rules.push_back(std::move(ready_rule));
  assigned_visual.triggers.push_back(
      {.cooldown_id = "dash", .observation_id = "ready"});
  history.reset(original, assigned_visual, no_automation);
  auto without_cooldown = original;
  without_cooldown.name = "Cooldown removed";
  auto without_visual = assigned_visual;
  without_visual.rules.clear();
  without_visual.triggers.clear();
  passed &= expect(
      history.record(without_cooldown, without_visual, no_automation, false),
      "a cooldown and visual-assignment removal should share one history "
      "transaction");
  const auto restored_bundle =
      history.undo(without_cooldown, without_visual, no_automation);
  passed &=
      expect(restored_bundle.has_value() &&
                 restored_bundle->profile.name == original.name &&
                 restored_bundle->visual_profile.has_value() &&
                 restored_bundle->visual_profile->triggers.size() == 1,
             "undo should restore a removed cooldown's visual assignment");

  history.reset(original, assigned_visual, no_automation);
  auto pending_base = original;
  pending_base.name = "Pending base edit";
  passed &= expect(
      history.record(pending_base, assigned_visual, no_automation, false),
      "a pending base edit should enter combined history");
  auto committed_visual = assigned_visual;
  committed_visual.triggers.front().enabled = false;
  passed &= expect(
      history.record(pending_base, committed_visual, no_automation, false),
      "a committed visual-only action should enter combined history");
  passed &= expect(
      history.is_dirty(pending_base, committed_visual, no_automation, true),
      "pending base and visual edits must stay dirty until an explicit save");
  const auto base_undo =
      history.undo(pending_base, committed_visual, no_automation);
  passed &= expect(
      base_undo.has_value() && base_undo->profile.name == pending_base.name &&
          base_undo->visual_profile == assigned_visual,
      "undo should restore the visual state before a visual-only commit");
  if (base_undo.has_value()) {
    const auto visual_redo =
        history.redo(base_undo->profile, base_undo->visual_profile,
                     base_undo->automation_profile);
    passed &= expect(
        visual_redo.has_value() && visual_redo->profile == pending_base &&
            visual_redo->visual_profile == committed_visual,
        "redo should deterministically restore the committed visual state");
  }

  layerc::core::AutomationProfile assigned_automation;
  assigned_automation.profile_id = original.id;
  layerc::core::AutomationDefinition automation;
  automation.id = "automation";
  automation.name = "Automation";
  assigned_automation.automations.push_back(automation);
  history.reset(original, assigned_visual, assigned_automation);
  auto disabled_automation = assigned_automation;
  disabled_automation.automations.front().enabled = false;
  passed &= expect(
      history.record(original, assigned_visual, disabled_automation, false),
      "an automation edit should enter the combined bundle history");
  const auto automation_undo =
      history.undo(original, assigned_visual, disabled_automation);
  passed &= expect(
      automation_undo.has_value() &&
          automation_undo->automation_profile.has_value() &&
          automation_undo->automation_profile->automations.front().enabled,
      "undo must restore the automation sidecar instead of dropping it");

  history.reset(original, no_visual, no_automation);
  auto bounded = original;
  for (int index = 0; index < 60; ++index) {
    bounded.name = "Edit " + std::to_string(index);
    (void)history.record(bounded, no_visual, no_automation, false);
  }
  int undo_count{};
  while (const auto previous =
             history.undo(bounded, no_visual, no_automation)) {
    bounded = previous->profile;
    ++undo_count;
  }
  passed &= expect(undo_count == 50,
                   "editor undo history should retain exactly 50 transactions");
  const auto normal_checkbox = layerc::editor::profile_checkbox_metrics(96);
  const auto high_dpi_checkbox = layerc::editor::profile_checkbox_metrics(192);
  passed &=
      expect(normal_checkbox.image_size == 20 && normal_checkbox.inset == 2 &&
                 normal_checkbox.pen_width == 2,
             "profile checkbox resources should retain their 96-DPI metrics");
  passed &= expect(
      high_dpi_checkbox.image_size == 40 && high_dpi_checkbox.inset == 4 &&
          high_dpi_checkbox.pen_width == 4,
      "profile checkbox resources and hit target should scale with DPI");
  const RECT profile_row{0, 20, 240, 44};
  passed &= expect(
      layerc::editor::profile_checkbox_hit(profile_row, {8, 30}, 96) &&
          layerc::editor::profile_checkbox_hit(profile_row, {19, 43}, 96),
      "a click on the profile checkbox cell should toggle enablement");
  passed &= expect(
      !layerc::editor::profile_checkbox_hit(profile_row, {20, 30}, 96) &&
          !layerc::editor::profile_checkbox_hit(profile_row, {125, 30}, 96) &&
          !layerc::editor::profile_checkbox_hit(profile_row, {8, 44}, 96),
      "a click on the profile name should never toggle enablement");
  passed &= expect(
      layerc::editor::profile_checkbox_hit(profile_row, {39, 30}, 192) &&
          !layerc::editor::profile_checkbox_hit(profile_row, {40, 30}, 192),
      "the profile checkbox hit cell should scale with DPI");
  const auto legacy_base = [](const std::string_view source) {
    return layerc::platform::source_schema_before(
        source, layerc::core::profile_schema_version);
  };
  const auto legacy_visual = [](const std::string_view source) {
    return layerc::platform::source_schema_before(
        source, layerc::core::visual_schema_version);
  };
  passed &=
      expect(legacy_base(R"({"schemaVersion":1,"name":"legacy"})") &&
                 legacy_base(R"({"schemaVersion":2,"name":"legacy"})") &&
                 legacy_base(R"({"schemaVersion":3,"name":"legacy"})") &&
                 !legacy_base(R"({"schemaVersion":4,"name":"current"})") &&
                 legacy_visual(R"({"schemaVersion":1})") &&
                 !legacy_visual(R"({"schemaVersion":2})") &&
                 !legacy_base("{broken"),
             "the editor should warn only when a valid source predates its "
             "current schema");

  layerc::platform::EditorAppearanceSettings initial_appearance;
  initial_appearance.theme = layerc::platform::EditorTheme::copper;
  initial_appearance.accent = 0xD88E4BU;
  layerc::editor::AppearanceDraft appearance_draft;
  appearance_draft.begin(initial_appearance);
  appearance_draft.staged().theme = layerc::platform::EditorTheme::teal;
  appearance_draft.staged().overrides.front().fields =
      layerc::platform::editor_fill;
  appearance_draft.cancel();
  passed &= expect(
      appearance_draft.staged().theme == initial_appearance.theme &&
          appearance_draft.staged().overrides.front().fields == 0,
      "cancelling appearance edits should restore the persisted settings");

  appearance_draft.staged().theme = layerc::platform::EditorTheme::plum;
  bool save_received_staged{};
  passed &= expect(
      appearance_draft.apply(
          [&save_received_staged](
              const layerc::platform::EditorAppearanceSettings &settings) {
            save_received_staged =
                settings.theme == layerc::platform::EditorTheme::plum;
            return true;
          }) &&
          save_received_staged,
      "applying appearance edits should persist the complete staged value");
  appearance_draft.staged().theme = layerc::platform::EditorTheme::dark;
  passed &= expect(
      !appearance_draft.apply(
          [](const layerc::platform::EditorAppearanceSettings &) {
            return false;
          }),
      "a failed appearance save should report failure");
  appearance_draft.cancel();
  passed &= expect(
      appearance_draft.staged().theme == layerc::platform::EditorTheme::plum,
      "a failed appearance save should preserve the last persisted settings");

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

  const auto edited_duration = layerc::editor::parse_duration_ms(L"7.25");
  passed &= expect(
      edited_duration == layerc::core::Milliseconds(7'250) &&
          !layerc::editor::parse_duration_ms(L"0").has_value() &&
          !layerc::editor::parse_duration_ms(L"-1").has_value(),
      "cooldown duration editing should accept positive decimals and reject "
      "non-positive values");
  passed &= expect(
      !layerc::editor::parse_duration_ms(L"+5").has_value() &&
          !layerc::editor::parse_duration_ms(L" 5").has_value() &&
          layerc::editor::parse_duration_ms(L"86400") ==
              layerc::core::max_cooldown_duration &&
          !layerc::editor::parse_duration_ms(L"86400.5").has_value(),
      "cooldown duration editing should reject signs, whitespace, and "
      "durations above the shared core limit");
  passed &= expect(
      layerc::editor::parse_bounded(L"FFFFFF", 0, 0xFFFFFFU, 16) ==
              0xFFFFFFU &&
          !layerc::editor::parse_bounded(L"-FFFFF", 0, 0xFFFFFFU, 16) &&
          !layerc::editor::parse_bounded(L"+FFFFF", 0, 0xFFFFFFU, 16) &&
          !layerc::editor::parse_bounded(L" FFFFF", 0, 0xFFFFFFU, 16) &&
          !layerc::editor::parse_bounded(L"256", 0, 255) &&
          !layerc::editor::parse_bounded(L"", 0, 255) &&
          layerc::editor::parse_uint(L"-1", 7) == 7 &&
          layerc::editor::parse_uint(L"42", 7) == 42,
      "editor number fields should reject signed, padded, and out-of-range "
      "input instead of wrapping it");
  layerc::core::GameProfile persisted_profile;
  persisted_profile.id = "cooldown-edit";
  persisted_profile.name = "Cooldown edit";
  persisted_profile.game.executable_name = "game.exe";
  persisted_profile.cooldowns.push_back(
      {.id = "ability",
       .name = "Ability",
       .duration = std::chrono::seconds(5),
       .max_charges = 1,
       .retrigger = layerc::core::RetriggerPolicy::ignore});
  persisted_profile.panels.push_back(
      {.id = "main",
       .name = "Main",
       .style = layerc::core::PanelStyle::bar,
       .bar = {},
       .anchor_x = 0.5,
       .anchor_y = 0.75,
       .cooldown_ids = {"ability"}});
  persisted_profile.bindings.push_back(
      {.cooldown_id = "ability",
       .chord = layerc::core::InputChord{{{
           layerc::core::InputDevice::keyboard, 0x12, 0}}}});
  persisted_profile.cooldowns.front().name = "Edited Ability";
  if (edited_duration.has_value())
    persisted_profile.cooldowns.front().duration = *edited_duration;
  const auto reloaded_result = layerc::core::parse_profile_json(
      layerc::core::serialize_profile_json(persisted_profile));
  const auto *reloaded_profile =
      std::get_if<layerc::core::GameProfile>(&reloaded_result);
  passed &= expect(
      reloaded_profile != nullptr && !reloaded_profile->cooldowns.empty() &&
          reloaded_profile->cooldowns.front().name == "Edited Ability" &&
          reloaded_profile->cooldowns.front().duration ==
              layerc::core::Milliseconds(7'250),
      "edited cooldown name and duration should survive save and reload");

  const HWND editor_dialog = CreateDialogParamW(
      instance, MAKEINTRESOURCEW(IDD_EDITOR), nullptr, &test_dialog_proc, 0);
  passed &= expect(editor_dialog != nullptr,
                   "the native V2.2 editor resource should load");
  if (editor_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        editor_dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HMENU editor_menu = GetMenu(editor_dialog);
    const HMENU file_menu = GetSubMenu(editor_menu, 0);
    const HMENU edit_menu = GetSubMenu(editor_menu, 1);
    const HMENU view_menu = GetSubMenu(editor_menu, 2);
    const HMENU tools_menu = GetSubMenu(editor_menu, 3);
    const HMENU help_menu = GetSubMenu(editor_menu, 4);
    passed &= expect(
        GetMenuItemCount(editor_menu) == 5 &&
            menu_caption(editor_menu, 0) == L"&File" &&
            menu_caption(editor_menu, 1) == L"&Edit" &&
            menu_caption(editor_menu, 2) == L"&View" &&
            menu_caption(editor_menu, 3) == L"&Tools" &&
            menu_caption(editor_menu, 4) == L"&Help",
        "the editor should expose native File, Edit, View, Tools, and Help "
        "menus");
    passed &= expect(
        menu_has_command(view_menu, IDM_VIEW_ADVANCED) &&
            menu_has_command(view_menu, IDM_VIEW_TEXT_130) &&
            menu_has_command(edit_menu, IDM_COOLDOWN_DUPLICATE) &&
            menu_has_command(file_menu, IDM_FILE_SAVE_AS) &&
            menu_has_command(help_menu, IDM_HELP_SHORTCUTS),
        "View, cooldown and shortcut commands should be reachable from menus");
    // Access keys within one popup must be unique (Export Pack vs Exit).
    std::vector<wchar_t> file_keys;
    for (int item = 0; item < GetMenuItemCount(file_menu); ++item) {
      const auto caption = menu_caption(file_menu, item);
      const auto amp = caption.find(L'&');
      if (amp != std::wstring::npos && amp + 1 < caption.size())
        file_keys.push_back(static_cast<wchar_t>(towlower(caption[amp + 1])));
    }
    std::ranges::sort(file_keys);
    passed &= expect(std::ranges::adjacent_find(file_keys) == file_keys.end(),
                     "File menu access keys should be unique");
    const HMENU context_menus =
        LoadMenuW(instance, MAKEINTRESOURCEW(IDR_CONTEXT_MENUS));
    passed &= expect(
        context_menus != nullptr &&
            menu_has_command(GetSubMenu(context_menus, 0), IDM_PROFILE_TOGGLE) &&
            menu_has_command(GetSubMenu(context_menus, 1), IDM_COOLDOWN_REMOVE),
        "profile and cooldown context menus should load");
    if (context_menus != nullptr)
      DestroyMenu(context_menus);
    passed &= expect(
        menu_has_command(file_menu, IDM_FILE_NEW) &&
            menu_has_command(file_menu, IDM_FILE_IMPORT_PACK) &&
            menu_has_command(file_menu, IDM_FILE_EXPORT_PACK) &&
            menu_has_command(edit_menu, IDM_EDIT_UNDO) &&
            menu_has_command(edit_menu, IDM_EDIT_REDO) &&
            menu_has_command(tools_menu, IDM_FILE_OPTIONS) &&
            menu_has_command(tools_menu, IDM_TOOLS_APPEARANCE) &&
            menu_has_command(tools_menu, IDM_FILE_AUTOMATION) &&
            menu_has_command(help_menu, IDM_FILE_RUNTIME_DIAGNOSTICS) &&
            menu_has_command(help_menu, IDM_HELP_ABOUT),
        "native menus should retain every V2.5 command launcher");
    passed &= expect(
        GetDlgItem(editor_dialog, IDC_CHOOSE_APPLICATION) != nullptr &&
            GetDlgItem(editor_dialog, IDC_ADVANCED) != nullptr &&
            GetDlgItem(editor_dialog, IDC_PROFILE_ID) != nullptr &&
            GetDlgItem(editor_dialog, IDC_APPLICATION_EXACT_PATH) != nullptr &&
            GetDlgItem(editor_dialog, IDC_COOLDOWN_NAME) != nullptr &&
            GetDlgItem(editor_dialog, IDC_COOLDOWN_DURATION) != nullptr &&
            GetDlgItem(editor_dialog, IDC_POSITION_ON_WINDOW) != nullptr,
        "the main resource should expose application, cooldown, advanced, and "
        "window-position controls");
    passed &= expect(
        GetDlgItem(editor_dialog, IDC_BROWSE_APPLICATION) != nullptr &&
            GetDlgItem(editor_dialog, IDC_PROFILE_NEW) != nullptr &&
            GetDlgItem(editor_dialog, IDC_PROFILE_DELETE) != nullptr &&
            GetDlgItem(editor_dialog, IDC_PROFILE_MORE) != nullptr &&
            GetDlgItem(editor_dialog, IDC_CONNECTION_STATUS) != nullptr &&
            GetDlgItem(editor_dialog, IDC_TIMING_LABEL) != nullptr &&
            GetDlgItem(editor_dialog, IDC_OVERLAY_LABEL) != nullptr,
        "the master-detail editor should expose profile actions, browsing, "
        "cooldown detail groups and the status strip");
    passed &= expect(
        (GetWindowLongPtrW(GetDlgItem(editor_dialog, IDC_APPLICATION_PATH),
                           GWL_STYLE) &
         ES_READONLY) == 0 &&
            (GetWindowLongPtrW(GetDlgItem(editor_dialog, IDC_COOLDOWN_BINDING),
                               GWL_STYLE) &
             WS_TABSTOP) == 0,
        "the application accepts typed names and read-only displays are not "
        "tab stops");
    // Tab order: the view toggle follows the profile fields.
    HWND walker = GetDlgItem(editor_dialog, IDC_APPLICATION_PATH);
    bool advanced_after_fields = false;
    for (int step = 0; step < 8 && walker != nullptr; ++step) {
      walker = GetWindow(walker, GW_HWNDNEXT);
      if (walker == GetDlgItem(editor_dialog, IDC_ADVANCED))
        advanced_after_fields = true;
    }
    passed &= expect(advanced_after_fields,
                     "Show advanced should follow the profile fields in tab "
                     "order");
    passed &= expect(
        (GetWindowLongPtrW(GetDlgItem(editor_dialog, IDC_COOLDOWN_DURATION),
                           GWL_STYLE) &
         ES_NUMBER) == 0,
        "cooldown duration should allow validated decimal seconds");
    const HWND assignment = GetDlgItem(editor_dialog, IDC_VISUAL_ENABLED);
    const HWND isolated = GetDlgItem(editor_dialog, IDC_VISUAL_ISOLATED);
    const auto assignment_type =
        GetWindowLongPtrW(assignment, GWL_STYLE) & BS_TYPEMASK;
    passed &= expect(
        assignment_type == BS_AUTOCHECKBOX,
        "the visual assignment checkbox must retain auto-toggle semantics");
    SendMessageW(assignment, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(assignment, BM_CLICK, 0, 0);
    passed &=
        expect(SendMessageW(assignment, BM_GETCHECK, 0, 0) == BST_CHECKED,
               "the visual assignment checkbox should toggle through BM_CLICK");
    passed &= expect(
        (GetWindowLongPtrW(isolated, GWL_STYLE) & BS_TYPEMASK) ==
            BS_AUTOCHECKBOX,
        "the isolated visual checkbox must retain auto-toggle semantics");
    SendMessageW(isolated, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(isolated, BM_CLICK, 0, 0);
    passed &=
        expect(SendMessageW(isolated, BM_GETCHECK, 0, 0) == BST_CHECKED,
               "the isolated visual checkbox should toggle through BM_CLICK");
    DestroyWindow(editor_dialog);
  }

  const HWND appearance_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_APPEARANCE), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      appearance_dialog != nullptr &&
          GetDlgItem(appearance_dialog, IDC_THEME_PRESET) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_ADVANCED) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_RESET_ALL) != nullptr &&
          GetDlgItem(appearance_dialog, IDC_APPEARANCE_APPLY) != nullptr,
      "Appearance should expose preset-first controls and staged actions");
  if (appearance_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        appearance_dialog,
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND advanced =
        GetDlgItem(appearance_dialog, IDC_APPEARANCE_ADVANCED);
    const auto advanced_type =
        GetWindowLongPtrW(advanced, GWL_STYLE) & BS_TYPEMASK;
    passed &= expect(
        advanced_type == BS_PUSHBUTTON || advanced_type == BS_OWNERDRAW,
        "advanced appearance colors must be a disclosure button, not a "
        "checkbox that looks like a saved setting");
    passed &= expect(
        LOWORD(SendMessageW(appearance_dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "OK should remain the Appearance default action");
    DestroyWindow(appearance_dialog);
  }

  const HWND tasks_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_TASKS), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      tasks_dialog != nullptr &&
          (GetWindowLongPtrW(tasks_dialog, GWL_STYLE) & WS_THICKFRAME) != 0 &&
          GetDlgItem(tasks_dialog, IDC_TASK_LIST) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_PICK_WINDOW) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_USE_TASK) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_REFRESH_TASKS) != nullptr &&
          GetDlgItem(tasks_dialog, IDC_TASK_ACTIONS) != nullptr,
      "Detected Tasks should remain resizable with all primary actions "
      "visible");
  if (tasks_dialog != nullptr)
    DestroyWindow(tasks_dialog);

  for (const int resource : {IDD_EXPORT_PACK, IDD_IMPORT_PACK}) {
    const HWND pack_dialog =
        CreateDialogParamW(instance, MAKEINTRESOURCEW(resource), nullptr,
                           &test_dialog_proc, 0);
    passed &= expect(
        pack_dialog != nullptr &&
            (GetWindowLongPtrW(pack_dialog, GWL_STYLE) & WS_THICKFRAME) != 0 &&
            GetDlgItem(pack_dialog, IDC_PACK_INTRO) != nullptr &&
            GetDlgItem(pack_dialog, IDC_PACK_LIST) != nullptr &&
            GetDlgItem(pack_dialog, IDC_PACK_CONFIRM) != nullptr,
        "pack dialogs should expose responsive list and action resources");
    if (pack_dialog != nullptr) {
      RECT client{};
      GetClientRect(pack_dialog, &client);
      const RECT list = child_rect(pack_dialog, IDC_PACK_LIST);
      const RECT confirm = child_rect(pack_dialog, IDC_PACK_CONFIRM);
      passed &= expect(
          (list.right - list.left) * 5 >= (client.right - client.left) * 4 &&
              list.bottom < confirm.top,
          "pack list should consume available width without covering actions");
      DestroyWindow(pack_dialog);
    }
  }

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
  const HWND hid_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_HID_BINDING), nullptr,
                         &test_dialog_proc, 0);
  passed &= expect(
      hid_dialog != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_DEVICE) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_CAPTURE) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_THRESHOLD) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_HYSTERESIS) != nullptr &&
          GetDlgItem(hid_dialog, IDC_HID_SLOT) != nullptr,
      "the HID capture/refine resource should expose every V2.2 control");
  if (hid_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        hid_dialog, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    const HWND advanced = GetDlgItem(hid_dialog, IDC_HID_SYSTEM_CLASS);
    passed &= expect(
        (GetWindowLongPtrW(advanced, GWL_STYLE) & BS_TYPEMASK) ==
            BS_AUTOCHECKBOX,
        "the system-class HID opt-in must retain explicit checkbox semantics");
    passed &= expect(LOWORD(SendMessageW(hid_dialog, DM_GETDEFID, 0, 0)) ==
                         IDC_HID_APPLY,
                     "Use Binding should remain the HID dialog default action");
    DestroyWindow(hid_dialog);
  }

  const HWND diagnostics_dialog =
      CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_RUNTIME_DIAGNOSTICS),
                         nullptr, &test_dialog_proc, 0);
  passed &= expect(
      diagnostics_dialog != nullptr &&
          (GetWindowLongPtrW(diagnostics_dialog, GWL_STYLE) & WS_THICKFRAME) !=
              0 &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_SUMMARY) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RAW_TOGGLE) !=
              nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_TEXT) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_REFRESH) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_COPY) != nullptr &&
          GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RETRY_VISUAL) !=
              nullptr,
      "runtime diagnostics should expose resizable summary, raw JSON, copy, "
      "and contextual retry controls");
  if (diagnostics_dialog != nullptr) {
    layerc::editor::style_native_dialog_children(
        diagnostics_dialog,
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    passed &= expect(
        (GetWindowLongPtrW(
             GetDlgItem(diagnostics_dialog, IDC_DIAGNOSTICS_RAW_TOGGLE),
             GWL_STYLE) &
         BS_TYPEMASK) == BS_AUTOCHECKBOX,
        "the raw diagnostics disclosure should retain native checkbox "
        "behavior");
    passed &= expect(
        LOWORD(SendMessageW(diagnostics_dialog, DM_GETDEFID, 0, 0)) == IDOK,
        "Close should remain the Runtime Diagnostics default action");
    DestroyWindow(diagnostics_dialog);
  }

  layerc::editor::TaskPicker task_picker_state;
  passed &= expect(!task_picker_state.has_request(),
                   "task picker should start without a target request");
  task_picker_state.request_live_edit();
  passed &= expect(task_picker_state.awaiting_live_edit() &&
                       !task_picker_state.awaiting_calibration(),
                   "live-edit target selection should be exclusive");
  task_picker_state.request_calibration();
  passed &= expect(task_picker_state.awaiting_calibration() &&
                       !task_picker_state.awaiting_live_edit(),
                   "calibration should replace any live-edit target request");
  task_picker_state.clear_request();
  passed &= expect(!task_picker_state.has_request(),
                   "clearing the task picker should remove its target request");

  layerc::editor::PanelPreview panel_state;
  panel_state.begin_drag({12, 24}, {3, 4});
  panel_state.mark_drag_moved();
  panel_state.finish_drag();
  passed &= expect(!panel_state.dragging && !panel_state.drag_moved &&
                       panel_state.drag_start.x == 0 &&
                       panel_state.drag_start.y == 0 &&
                       panel_state.drag_offset.x == 0 &&
                       panel_state.drag_offset.y == 0,
      "finishing a preview drag should reset the entire drag state");

  layerc::editor::CooldownEditor binding_state;
  binding_state.begin_binding_capture();
  binding_state.record_binding_key(0x002AU);
  binding_state.record_binding_key(0x002AU);
  binding_state.record_binding_key(0x0012U);
  const auto &captured_codes = binding_state.pending_binding_codes();
  passed &= expect(binding_state.capturing_binding() &&
                       captured_codes.size() == 2U &&
                       captured_codes[0] == 0x002AU &&
                       captured_codes[1] == 0x0012U,
                   "binding capture should retain an ordered chord while "
                   "ignoring key-repeat duplicates");
  binding_state.finish_binding_capture();
  passed &= expect(!binding_state.capturing_binding() &&
                       binding_state.pending_binding_codes().empty(),
                   "finishing binding capture should clear pending keys");

  DestroyWindow(dialog);
}

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

void editor_draft_leave_contracts();
void editor_dialog_font_contracts();

int main() {
  return layerc::test::run(
      {{"editor_dialog_contracts", editor_dialog_contracts},
       {"automation_palette_contracts", automation_palette_contracts},
       {"editor_draft_leave_contracts", editor_draft_leave_contracts},
       {"editor_dialog_font_contracts", editor_dialog_font_contracts}});
}
