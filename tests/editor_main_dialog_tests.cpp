#include "dialog_layout.hpp"
#include "editor_dialog_tests.hpp"
#include "editor_history.hpp"
#include "editor_resource.h"
#include "editor_state.hpp"
#include "editor_text.hpp"
#include "editor_util.hpp"
#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

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

} // namespace

namespace layerc::test::editor_dialog {

void profile_history_contracts(bool &passed) {
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
  assigned_visual.observations.emplace_back(std::move(ready_rule));
  assigned_visual.triggers.push_back(
      {.cooldown_id = "dash", .observation_id = "ready"});
  history.reset(original, assigned_visual, no_automation);
  auto without_cooldown = original;
  without_cooldown.name = "Cooldown removed";
  auto without_visual = assigned_visual;
  without_visual.observations.clear();
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
}

void profile_checkbox_contracts(bool &passed) {
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
}

void source_schema_contracts(bool &passed) {
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
}

void cooldown_edit_contracts(bool &passed) {
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
}

void main_dialog_contracts(const HINSTANCE instance, bool &passed) {
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
}

void panel_preview_contracts(bool &passed) {
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
}

void binding_capture_contracts(bool &passed) {
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
}

} // namespace layerc::test::editor_dialog
