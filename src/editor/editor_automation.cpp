#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_automation_internal.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <variant>

#include <commctrl.h>
#include <commdlg.h>
#include <windows.h>

namespace layerc::editor {
namespace {
using namespace std::chrono_literals;


// One list with the core wire format: combo order, label lookup, and the
// simulator all index the same canonical ordering.
constexpr auto &automation_event_kinds = layerc::core::automation_event_kinds;

constexpr auto &automation_actions = layerc::core::automation_action_specs;


struct JsonErrorPresentation {
  std::wstring message;
  std::optional<LONG> selection_start;
  std::optional<LONG> selection_end;
};

JsonErrorPresentation
present_json_error(const std::string_view source,
                   const layerc::core::AutomationError &error) {
  const auto location = locate_json_error(source, error);
  JsonErrorPresentation result{.message = to_wide(location.message),
                               .selection_start = std::nullopt,
                               .selection_end = std::nullopt};
  if (location.begin && location.end) {
    result.selection_start =
        static_cast<LONG>(to_wide(source.substr(0, *location.begin)).size());
    const auto selected = to_wide(source.substr(
        *location.begin, *location.end - *location.begin)).size();
    result.selection_end = *result.selection_start +
        static_cast<LONG>(std::max<std::size_t>(1, selected));
  }
  return result;
}

void select_json_error(const HWND dialog,
                       const JsonErrorPresentation &presentation) {
  if (!presentation.selection_start || !presentation.selection_end)
    return;
  const HWND editor = GetDlgItem(dialog, IDC_AUTOMATION_JSON);
  SetFocus(editor);
  SendMessageW(editor, EM_SETSEL, *presentation.selection_start,
               *presentation.selection_end);
  SendMessageW(editor, EM_SCROLLCARET, 0, 0);
}

} // namespace

EditorWindow::EditorWindow()
    : cooldown_editor_(
          document_, shell_, static_cast<const DialogTheme &>(*this),
          [this](const ModuleEditOutcome &outcome, const DocumentChangeKind kind) {
            return apply_edit(outcome, kind);
          },
          {.sync_fields = [this] { (void)apply_profile_controls(DocumentChangeKind::text); },
           .follow_bar = [this] { follow_cooldown_bar(); },
           .refresh_visual = [this] { visual_editor_.refresh(); },
           .refresh_bars =
               [this](const std::size_t preferred_panel) {
                 refresh_bar_list(preferred_panel);
               }}),
      visual_editor_(
          document_, shell_, live_edit_, task_picker_,
          {.selected_cooldown =
               [this]() -> const layerc::core::CooldownDefinition * {
                 return cooldown_editor_.selected();
               },
           .commit_visual =
               [this](layerc::core::VisualProfile visual) {
                 return commit_document(
                     document_.path(), {},
                     [visual = std::move(visual)](ProfileBundle &bundle) {
                       bundle.visual = visual;
                     });
               },
           .mark_committed = [this] { mark_committed(); },
           .write_failure_message = [this] { return write_failure_message(); },
           .require_stored_profile =
               [this](const wchar_t *first_save_message) {
                 return require_stored_profile(first_save_message);
               },
           .show_tasks = [this] { show_tasks(); }}),
      automation_editor_(std::make_unique<AutomationEditor>(
          document_, live_edit_, shell_,
          static_cast<const DialogTheme &>(*this),
          [this](const ModuleEditOutcome &outcome, const DocumentChangeKind kind) {
            return apply_edit(outcome, kind);
          })) {}

EditorWindow::~EditorWindow() = default;

int AutomationEditor::selected_index(const HWND dialog) const {
  const auto selected =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_GETCURSEL, 0, 0);
  const auto &profile = document_.automation();
  return selected == CB_ERR || selected < 0 || !profile ||
                 static_cast<std::size_t>(selected) >= profile->automations.size()
             ? -1
             : static_cast<int>(selected);
}

const layerc::core::AutomationDefinition *
AutomationEditor::selected_automation(const HWND dialog) const {
  const int index = selected_index(dialog);
  return index < 0 ? nullptr
                   : &document_.automation()
                          ->automations[static_cast<std::size_t>(index)];
}

const layerc::core::AutomationRule *
AutomationEditor::selected_rule(const HWND dialog) const {
  const auto *automation = selected_automation(dialog);
  return automation && node_rule_ >= 0 &&
                 static_cast<std::size_t>(node_rule_) < automation->rules.size()
             ? &automation->rules[static_cast<std::size_t>(node_rule_)]
             : nullptr;
}

const layerc::core::AutomationAction *
AutomationEditor::selected_action(const HWND dialog) const {
  const auto *rule = selected_rule(dialog);
  return rule && node_action_ >= 0 &&
                 static_cast<std::size_t>(node_action_) < rule->actions.size()
             ? &rule->actions[static_cast<std::size_t>(node_action_)]
             : nullptr;
}

AutomationEditor::Mutation
AutomationEditor::mutate_automation(const HWND dialog, const wchar_t *title,
                                    const AutomationEdit &edit,
                                    const bool repair) {
  const auto outcome =
      document_.edit_automation(edit, DocumentChangeKind::immediate, repair);
  switch (outcome.status) {
  case ModuleEditStatus::blocked:
  case ModuleEditStatus::refused:
    themed_message_box(dialog, to_wide(outcome.message).c_str(), title,
                       MB_OK | MB_ICONWARNING);
    return Mutation::rejected;
  case ModuleEditStatus::invalid:
    themed_message_box(dialog, to_wide(outcome.message).c_str(), title,
                       MB_OK | MB_ICONWARNING);
    return Mutation::invalid;
  case ModuleEditStatus::accepted:
    (void)report_edit_(outcome, DocumentChangeKind::immediate);
    return Mutation::committed;
  case ModuleEditStatus::unchanged:
    return Mutation::committed;
  }
  return Mutation::rejected;
}

void AutomationEditor::refresh(const HWND dialog) {
  const int preferred = selected_index(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_RESETCONTENT, 0, 0);
  if (document_.automation()) {
    for (const auto &automation : document_.automation()->automations) {
      const auto label = to_wide(automation_label(automation));
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_ADDSTRING, 0,
                          reinterpret_cast<LPARAM>(label.c_str()));
    }
  }

  const int count =
      document_.automation()
          ? static_cast<int>(document_.automation()->automations.size())
          : 0;
  const int selected = count == 0 ? -1 : std::clamp(preferred, 0, count - 1);
  if (selected >= 0)
    SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_SETCURSEL, selected, 0);
  const auto *definition = selected_automation(dialog);
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_ENABLED),
                          definition != nullptr && definition->enabled);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_NAME),
           definition == nullptr ? L"" : to_wide(definition->name));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON),
           to_wide(automation_graph_json(document_.automation())));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
           trace_window_text(trace_display_));
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_REMOVE),
               definition != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ENABLED),
               definition != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_NAME), definition != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_RETRY), definition != nullptr);
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE),
                          trace_enabled_);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_ENABLED), nullptr, TRUE);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), nullptr, TRUE);
  refresh_nodes(dialog);
}

void AutomationEditor::refresh_node_properties(const HWND dialog) {
  const auto *rule = selected_rule(dialog);
  const auto *action = selected_action(dialog);
  advanced_expression_displayed_ = false;
  for (const int id :
       {IDC_AUTOMATION_RULE_ID, IDC_AUTOMATION_EVENT, IDC_AUTOMATION_SOURCE,
        IDC_AUTOMATION_PRIORITY, IDC_AUTOMATION_APPLY_NODE,
        IDC_AUTOMATION_DELETE_NODE})
    EnableWindow(GetDlgItem(dialog, id), rule != nullptr);
  for (const int id :
       {IDC_AUTOMATION_ACTION_KIND, IDC_AUTOMATION_TARGET,
        IDC_AUTOMATION_VALUE, IDC_AUTOMATION_REPEATING})
    EnableWindow(GetDlgItem(dialog, id), action != nullptr);
  update_add_enabled(dialog);
  const auto *definition = selected_automation(dialog);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND),
           rule == nullptr
               ? (definition == nullptr
                      ? L"No automation yet — choose a preset above and press New."
                  : definition->rules.empty()
                      ? L"No rules yet — choose a rule type and press Add."
                      : L"Select a rule or action in the list.")
               : action == nullptr
                   ? L"Event rule"
                   : L"Action " + std::to_wstring(node_action_ + 1) +
                         L" in selected rule");
  const auto fields = document_.automation()
      ? automation_node_fields(*document_.automation(),
                               {selected_index(dialog), node_rule_, node_action_})
      : AutomationNodeFields{};
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_RULE_ID), to_wide(fields.rule_id));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SOURCE), to_wide(fields.source_filter));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_PRIORITY), to_wide(fields.priority));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET), to_wide(fields.target));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_VALUE), to_wide(fields.value));
  // Set after set_text: its EN_CHANGE clears the flag.
  advanced_expression_displayed_ = fields.advanced_value;
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_EVENT, CB_SETCURSEL,
                      static_cast<WPARAM>(fields.event_index), 0);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_ACTION_KIND, CB_SETCURSEL,
                      static_cast<WPARAM>(fields.action_kind_index), 0);
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING),
                          fields.repeating);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING), nullptr, TRUE);
}

void AutomationEditor::select_node(const HWND dialog) {
  const auto item =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_NODES, LB_GETCURSEL, 0, 0);
  if (item == LB_ERR)
    return;
  const auto data = SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_NODES, LB_GETITEMDATA,
      static_cast<WPARAM>(item), 0);
  if (data == LB_ERR)
    return;
  node_rule_ = automation_tree_rule(data);
  node_action_ = automation_tree_action(data);
  refresh_node_properties(dialog);
}

void AutomationEditor::apply_node_properties(const HWND dialog) {
  if (selected_rule(dialog) == nullptr)
    return;
  const auto text = [dialog](const int id) {
    return wide_to_utf8(window_text(GetDlgItem(dialog, id)));
  };
  AutomationNodeFields fields;
  fields.rule_id = text(IDC_AUTOMATION_RULE_ID);
  fields.event_index = static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_EVENT, CB_GETCURSEL, 0, 0));
  fields.source_filter = text(IDC_AUTOMATION_SOURCE);
  fields.priority = text(IDC_AUTOMATION_PRIORITY);
  fields.action_kind_index = static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_ACTION_KIND, CB_GETCURSEL, 0, 0));
  fields.target = text(IDC_AUTOMATION_TARGET);
  fields.value = text(IDC_AUTOMATION_VALUE);
  fields.repeating = owner_draw_checkbox_checked(
      GetDlgItem(dialog, IDC_AUTOMATION_REPEATING));
  const auto result = mutate_automation(
      dialog, L"Rule properties",
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        return apply_automation_node_fields(
            *candidate,
            {selected_index(dialog), node_rule_, node_action_}, fields,
            advanced_expression_displayed_);
      });
  // A refused value keeps the typed field text; committed and invalid edits
  // reload the properties from the profile.
  if (result != Mutation::rejected)
    refresh(dialog);
}

void AutomationEditor::delete_node(const HWND dialog) {
  if (selected_rule(dialog) == nullptr)
    return;
  AutomationSelection selection{selected_index(dialog), node_rule_, node_action_};
  const auto result = mutate_automation(
      dialog, L"Delete node",
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        selection = delete_automation_node(candidate, selection);
        return std::nullopt;
      }, true);
  if (result == Mutation::committed) {
    node_rule_ = selection.rule;
    node_action_ = selection.action;
    refresh(dialog);
  }
}

void AutomationEditor::initialize(const HWND dialog) {
  dialog_ = dialog;
  resize_to_logical_size(dialog, 1000, 700);
  apply_dialog_chrome(dialog, shell_.capture_exclusion);
  if (!chrome_.attach(dialog))
    style_native_dialog_children(dialog, shell_.font);
  chrome_.set_monospace({IDC_AUTOMATION_JSON, IDC_AUTOMATION_TRACE_TEXT});
  // Hints for the free-text fields (Common Controls 6 cue banners).
  const auto cue = [dialog](const int id, const wchar_t *text) {
    SendMessageW(GetDlgItem(dialog, id), EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(text));
  };
  cue(IDC_AUTOMATION_RULE_ID, L"Unique name, e.g. dash-start");
  cue(IDC_AUTOMATION_SOURCE, L"Any source, or a device or cooldown id");
  cue(IDC_AUTOMATION_TARGET, L"Cooldown or bar id the action changes");
  cue(IDC_AUTOMATION_VALUE, L"Number, text, true or false");
  cue(IDC_AUTOMATION_PRIORITY, L"0");
  cue(IDC_AUTOMATION_SIM_SOURCE, L"Any source");
  cue(IDC_AUTOMATION_SIM_FIELDS, L"name=JSON; name:type=JSON; ...");
  cue(IDC_AUTOMATION_SIM_BREAKPOINT, L"Rule id to pause at");
  cue(IDC_AUTOMATION_SIM_ASSERTIONS, L"id=JSON; ...");

  const HWND presets = GetDlgItem(dialog, IDC_AUTOMATION_PRESETS);
  for (const auto entry : automation_preset_labels)
    SendMessageW(presets, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(entry).c_str()));
  SendMessageW(presets, CB_SETCURSEL, 0, 0);

  const HWND views = GetDlgItem(dialog, IDC_AUTOMATION_VIEW);
  for (const auto entry : automation_view_labels)
    SendMessageW(views, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(entry).c_str()));
  SendMessageW(views, CB_SETCURSEL, 0, 0);

  const HWND property_event = GetDlgItem(dialog, IDC_AUTOMATION_EVENT);
  for (const auto kind : automation_event_kinds) {
    const auto label = to_wide(event_label(kind));
    SendMessageW(property_event, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  const HWND property_action = GetDlgItem(dialog, IDC_AUTOMATION_ACTION_KIND);
  for (const auto &action : automation_actions) {
    const auto label = to_wide(action_label(action.kind));
    SendMessageW(property_action, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }

  const HWND palette = GetDlgItem(dialog, IDC_AUTOMATION_PALETTE);
  for (const auto &entry : automation_palette)
    SendMessageW(palette, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(to_wide(entry.label).c_str()));
  SendMessageW(palette, CB_SETCURSEL, 0, 0);
  const HWND event = GetDlgItem(dialog, IDC_AUTOMATION_SIM_EVENT);
  for (const auto kind : automation_event_kinds) {
    const auto label = to_wide(event_label(kind));
    SendMessageW(event, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  SendMessageW(event, CB_SETCURSEL, 0, 0);
  const HWND speed = GetDlgItem(dialog, IDC_AUTOMATION_SIM_SPEED);
  for (const auto entry : simulator_speed_labels)
    SendMessageW(speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(to_wide(entry).c_str()));
  SendMessageW(speed, CB_SETCURSEL, 1, 0);
  update_view(dialog);
  refresh(dialog);
}

AutomationEditor::AutomationEditor(ProfileDocument &document,
                                   LiveEditSession &live_edit,
                                   const EditorShell &shell,
                                   const DialogTheme &theme,
                                   EditReporter report_edit) noexcept
    : document_(document), live_edit_(live_edit), shell_(shell), theme_(theme),
      report_edit_(std::move(report_edit)) {}
AutomationEditor::~AutomationEditor() = default;

void AutomationEditor::open() {
  DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_AUTOMATION), shell_.window,
      &dispatch_member_dialog<AutomationEditor, &AutomationEditor::initialize,
                              &AutomationEditor::handle_message>,
      reinterpret_cast<LPARAM>(this));
  RedrawWindow(shell_.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AutomationEditor::add_basic(const HWND dialog) {
  const int preset = std::max(0, static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_PRESETS, CB_GETCURSEL, 0, 0)));
  AutomationSelection selection;
  const auto result = mutate_automation(
      dialog, L"Automation",
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        const auto added = add_automation_preset(
            document_.profile(), candidate,
            static_cast<AutomationPreset>(preset));
        if (const auto *refusal = std::get_if<std::string>(&added))
          return *refusal;
        selection = std::get<AutomationSelection>(added);
        return std::nullopt;
      });
  if (result != Mutation::committed)
    return;
  refresh(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_SETCURSEL,
                      static_cast<WPARAM>(selection.automation), 0);
  node_rule_ = selection.rule;
  node_action_ = selection.action;
  refresh(dialog);
}
// Adding is offered only when it can succeed: an action needs a selected
// rule, some entries need a cooldown or an overlay bar, and non-blank
// presets need a cooldown.
void AutomationEditor::update_add_enabled(const HWND dialog) {
  const auto *entry = automation_palette_entry(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PALETTE, CB_GETCURSEL, 0, 0));
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ADD_NODE),
               entry && !palette_unavailable(
                   document_.profile(), document_.automation(),
                   {selected_index(dialog), node_rule_, node_action_}, *entry));
  const int preset = std::max(0, static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_PRESETS, CB_GETCURSEL, 0, 0)));
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ADD_PRESET),
               !preset_unavailable(document_.profile(),
                                   static_cast<AutomationPreset>(preset)));
}

void AutomationEditor::add_palette_node(const HWND dialog) {
  const int selected = selected_index(dialog);
  if (selected < 0) {
    add_basic(dialog);
    return;
  }
  const auto *entry = automation_palette_entry(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PALETTE, CB_GETCURSEL, 0, 0));
  if (!entry)
    return;
  AutomationSelection selection{selected, node_rule_, node_action_};
  const auto result = mutate_automation(
      dialog, L"Could not add the rule or action",
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        const auto added = layerc::editor::add_palette_node(
            document_.profile(), candidate, selection, *entry);
        if (const auto *refusal = std::get_if<std::string>(&added))
          return *refusal;
        selection = std::get<AutomationSelection>(added);
        return std::nullopt;
      });
  if (result != Mutation::committed)
    return;
  node_rule_ = selection.rule;
  node_action_ = selection.action;
  refresh(dialog);
}

void AutomationEditor::remove_selected(const HWND dialog) {
  if (selected_index(dialog) < 0)
    return;
  const auto result = mutate_automation(
      dialog, L"Remove automation",
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        (void)remove_automation(candidate,
                                {selected_index(dialog), -1, -1});
        return std::nullopt;
      }, true);
  if (result == Mutation::committed) {
    node_rule_ = -1;
    node_action_ = -1;
    refresh(dialog);
  }
}

bool AutomationEditor::apply_json(const HWND dialog) {
  const auto source =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON)));
  std::optional<layerc::core::AutomationError> json_error;
  const auto outcome = document_.edit_automation(
      [&](std::optional<layerc::core::AutomationProfile> &candidate)
          -> std::optional<std::string> {
        auto parsed = layerc::core::parse_automation_profile_json(
            source, document_.profile());
        if (const auto *error = std::get_if<layerc::core::AutomationError>(&parsed)) {
          json_error = *error;
          return error->describe();
        }
        candidate = std::move(std::get<layerc::core::AutomationProfile>(parsed));
        return std::nullopt;
      },
      DocumentChangeKind::immediate);
  if (json_error) {
    const auto presentation = present_json_error(source, *json_error);
    themed_message_box(dialog, presentation.message.c_str(),
                       L"Invalid automation graph", MB_OK | MB_ICONWARNING);
    select_json_error(dialog, presentation);
    return false;
  }
  if (outcome.status != ModuleEditStatus::accepted &&
      outcome.status != ModuleEditStatus::unchanged) {
    themed_message_box(dialog, to_wide(outcome.message).c_str(),
                       L"Invalid automation graph", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (outcome.accepted()) {
    node_rule_ = -1;
    node_action_ = -1;
    (void)report_edit_(outcome, DocumentChangeKind::immediate);
    refresh(dialog);
  }
  return true;
}

void AutomationEditor::validate_json(const HWND dialog) {
  const auto source =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON)));
  const auto parsed =
      layerc::core::parse_automation_profile_json(source, document_.profile());
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_RESETCONTENT, 0, 0);
  std::wstring line = L"Valid - ready to apply";
  std::optional<JsonErrorPresentation> presentation;
  if (const auto *error = std::get_if<layerc::core::AutomationError>(&parsed)) {
    presentation = present_json_error(source, *error);
    line = presentation->message;
  }
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_ADDSTRING, 0,
                      reinterpret_cast<LPARAM>(line.c_str()));
  if (presentation)
    select_json_error(dialog, *presentation);
}

bool AutomationEditor::handle_command(const HWND dialog,
                                      const WPARAM wparam) {
  const int id = LOWORD(wparam);
  const int notification = HIWORD(wparam);
  switch (id) {
  case IDC_AUTOMATION_LIST:
    if (notification == CBN_SELCHANGE) {
      node_rule_ = -1;
      node_action_ = -1;
      refresh(dialog);
    }
    return true;
  case IDC_AUTOMATION_PRESETS:
  case IDC_AUTOMATION_PALETTE:
    if (notification == CBN_SELCHANGE)
      update_add_enabled(dialog);
    return true;
  case IDC_AUTOMATION_VIEW:
    if (notification == CBN_SELCHANGE)
      update_view(dialog);
    return true;
  case IDC_AUTOMATION_NODES:
    if (notification == LBN_SELCHANGE)
      select_node(dialog);
    return true;
  case IDC_AUTOMATION_VALIDATION:
    if (notification == LBN_SELCHANGE) {
      validation_expanded_ = !validation_expanded_;
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_SETCURSEL,
                          static_cast<WPARAM>(-1), 0);
      layout(dialog);
    }
    return true;
  case IDC_AUTOMATION_VALUE:
    // Any edit replaces the advanced-expression placeholder with typed input.
    // Not consumed, so default dialog handling is unchanged.
    if (notification == EN_CHANGE)
      advanced_expression_displayed_ = false;
    return false;
  case IDC_AUTOMATION_APPLY_NODE:
    apply_node_properties(dialog);
    return true;
  case IDC_AUTOMATION_DELETE_NODE:
    delete_node(dialog);
    return true;
  case IDC_AUTOMATION_ADD_PRESET:
    add_basic(dialog);
    return true;
  case IDC_AUTOMATION_REMOVE:
    remove_selected(dialog);
    return true;
  case IDC_AUTOMATION_ENABLED:
    if (notification == BN_CLICKED) {
      const int index = selected_index(dialog);
      const auto outcome = document_.edit_automation(
          [&](std::optional<layerc::core::AutomationProfile> &automation)
              -> std::optional<std::string> {
            if (!automation || index < 0 ||
                static_cast<std::size_t>(index) >= automation->automations.size())
              return std::string("No automation is selected.");
            automation->automations[static_cast<std::size_t>(index)].enabled =
                owner_draw_checkbox_checked(
                    GetDlgItem(dialog, IDC_AUTOMATION_ENABLED));
            return std::nullopt;
          },
          DocumentChangeKind::immediate);
      if (report_edit_(outcome, DocumentChangeKind::immediate)) refresh(dialog);
    }
    return true;
  case IDC_AUTOMATION_NAME:
    if (notification == EN_KILLFOCUS && document_.automation()) {
      const int index = selected_index(dialog);
      const auto outcome = document_.edit_automation(
          [&](std::optional<layerc::core::AutomationProfile> &automation)
              -> std::optional<std::string> {
            if (!automation || index < 0 ||
                static_cast<std::size_t>(index) >= automation->automations.size())
              return std::string("No automation is selected.");
            automation->automations[static_cast<std::size_t>(index)].name =
                wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_NAME)));
            return std::nullopt;
          },
          DocumentChangeKind::text);
      (void)report_edit_(outcome, DocumentChangeKind::text);
      refresh(dialog);
    }
    return true;
  case IDC_AUTOMATION_APPLY:
    (void)apply_json(dialog);
    return true;
  case IDC_AUTOMATION_ADD_NODE:
    add_palette_node(dialog);
    return true;
  case IDC_AUTOMATION_VALIDATE:
    validate_json(dialog);
    return true;
  case IDC_AUTOMATION_TRACE_LOAD:
    load_trace(dialog);
    return true;
  case IDC_AUTOMATION_SIM_ADD:
    add_simulator_event(dialog);
    return true;
  case IDC_AUTOMATION_SIM_RESET:
    reset_simulator(dialog);
    return true;
  case IDC_AUTOMATION_SIM_STEP:
    step_simulator(dialog);
    return true;
  case IDC_AUTOMATION_SIM_PLAY:
    play_simulator(dialog, !simulator_playing_);
    return true;
  case IDC_AUTOMATION_TRACE:
    if (notification == BN_CLICKED)
      set_trace(dialog, owner_draw_checkbox_checked(
                            GetDlgItem(dialog, IDC_AUTOMATION_TRACE)));
    return true;
  case IDC_AUTOMATION_SAVE_TRACE:
    save_trace(dialog);
    return true;
  case IDC_AUTOMATION_RETRY:
    retry_selected(dialog);
    return true;
  case IDOK:
  case IDCANCEL:
    if (trace_enabled_)
      set_trace(dialog, false);
    dialog_ = nullptr;
    EndDialog(dialog, id);
    return true;
  default:
    return false;
  }
}

LRESULT AutomationEditor::handle_message(const HWND window, const UINT message,
                                         const WPARAM wparam,
                                         const LPARAM lparam) {
  if (message == WM_COMMAND && handle_command(window, wparam))
    return TRUE;
  if (message == WM_SIZE) {
    layout(window);
    return TRUE;
  }
  if (message == WM_DPICHANGED) {
    // The measured minimum belongs to the old DPI; the new layout remeasures.
    minimum_client_ = {};
    (void)chrome_.on_dpi_changed(HIWORD(wparam), lparam);
    layout(window);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO) {
    chrome_.on_minimum_size(lparam, 780, 520);
    apply_minimum_client_size(window, lparam, minimum_client_);
    return TRUE;
  }
  if (message == WM_TIMER && wparam == automation_simulator_timer) {
    step_simulator(window);
    return TRUE;
  }
  if (message == WM_DRAWITEM) {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (item.CtlType == ODT_LISTBOX)
      theme_.draw_list_box_item(item);
    else
      return theme_.dialog_theme_message(window, message, wparam, lparam)
          .value_or(FALSE);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    KillTimer(window, automation_simulator_timer);
    simulator_playing_ = false;
    dialog_ = nullptr;
    chrome_.on_destroy();
    return TRUE;
  }
  return theme_.dialog_theme_message(window, message, wparam, lparam)
      .value_or(FALSE);
}

} // namespace layerc::editor
