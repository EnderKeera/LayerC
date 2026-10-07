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

// First "<prefix>-<n>" id unused by items, counting up from items.size() + 1.
template <typename Items>
std::string numbered_unique_id(const std::string &prefix, const Items &items) {
  for (std::size_t suffix = items.size() + 1U;; ++suffix) {
    auto id = prefix + "-" + std::to_string(suffix);
    if (std::ranges::none_of(items,
                             [&](const auto &item) { return item.id == id; }))
      return id;
  }
}

std::wstring
automation_label(const layerc::core::AutomationDefinition &automation) {
  auto result =
      to_wide(automation.name.empty() ? automation.id : automation.name);
  if (!automation.enabled)
    result += L" (disabled)";
  return result;
}

// One list with the core wire format: combo order, label lookup, and the
// simulator all index the same canonical ordering.
constexpr auto &automation_event_kinds = layerc::core::automation_event_kinds;

constexpr auto &automation_actions = layerc::core::automation_action_specs;

// Display-only placeholder for a non-literal action value; editing state is
// tracked by AutomationEditor::advanced_expression_displayed_.
constexpr wchar_t kAdvancedExpressionText[] = L"<Advanced expression>";

struct JsonErrorPresentation {
  std::wstring message;
  std::optional<LONG> selection_start;
  std::optional<LONG> selection_end;
};

JsonErrorPresentation
present_json_error(const std::string_view source,
                   const layerc::core::AutomationError &error) {
  if (!error.path.starts_with("$@") || error.path.size() == 2)
    return {.message = to_wide(error.describe()),
            .selection_start = std::nullopt,
            .selection_end = std::nullopt};

  std::size_t byte_offset{};
  for (const char digit : std::string_view(error.path).substr(2)) {
    if (!std::isdigit(static_cast<unsigned char>(digit)))
      return {.message = to_wide(error.describe()),
              .selection_start = std::nullopt,
              .selection_end = std::nullopt};
    byte_offset = byte_offset * 10U + static_cast<std::size_t>(digit - '0');
    if (byte_offset > source.size())
      byte_offset = source.size();
  }

  const auto prefix = source.substr(0, byte_offset);
  const auto line = 1U + static_cast<std::size_t>(
                             std::ranges::count(prefix, '\n'));
  const auto newline = prefix.rfind('\n');
  const auto line_prefix =
      newline == std::string_view::npos ? prefix : prefix.substr(newline + 1U);
  const auto column = to_wide(line_prefix).size() + 1U;
  const auto wide_prefix = to_wide(prefix);
  const LONG start = static_cast<LONG>(wide_prefix.size());

  std::size_t next_bytes{};
  if (byte_offset < source.size()) {
    const auto lead = static_cast<unsigned char>(source[byte_offset]);
    if (lead < 0x80U)
      next_bytes = 1U;
    else if ((lead & 0xE0U) == 0xC0U)
      next_bytes = 2U;
    else if ((lead & 0xF0U) == 0xE0U)
      next_bytes = 3U;
    else
      next_bytes = 4U;
    next_bytes = std::min(next_bytes, source.size() - byte_offset);
  }
  const auto selected = to_wide(source.substr(byte_offset, next_bytes)).size();
  return {
      .message = L"Line " + std::to_wstring(line) + L", column " +
                 std::to_wstring(column) + L": " + to_wide(error.message) +
                 L" (" + to_wide(error.path) + L")",
      .selection_start = start,
      .selection_end = start + static_cast<LONG>(std::max<std::size_t>(1, selected)),
  };
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
          [this](const DocumentChangeKind kind) { record_profile_change(kind); },
          {.sync_fields = [this] { sync_profile_from_controls(); },
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
           .commit_document =
               [this] { return commit_document(document_.path()); },
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
          [this](const DocumentChangeKind kind) {
            record_profile_change(kind);
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

layerc::core::AutomationDefinition *
AutomationEditor::selected_automation(const HWND dialog) const {
  const int index = selected_index(dialog);
  return index < 0 ? nullptr
                   : &document_.automation()
                          ->automations[static_cast<std::size_t>(index)];
}

layerc::core::AutomationRule *
AutomationEditor::selected_rule(const HWND dialog) const {
  auto *automation = selected_automation(dialog);
  return automation && node_rule_ >= 0 &&
                 static_cast<std::size_t>(node_rule_) < automation->rules.size()
             ? &automation->rules[static_cast<std::size_t>(node_rule_)]
             : nullptr;
}

layerc::core::AutomationAction *
AutomationEditor::selected_action(const HWND dialog) const {
  auto *rule = selected_rule(dialog);
  return rule && node_action_ >= 0 &&
                 static_cast<std::size_t>(node_action_) < rule->actions.size()
             ? &rule->actions[static_cast<std::size_t>(node_action_)]
             : nullptr;
}

AutomationEditor::Mutation
AutomationEditor::mutate_automation(const HWND dialog, const wchar_t *title,
                                    const AutomationEdit &edit) {
  auto &profile = document_.automation();
  if (!profile)
    return Mutation::rejected;
  auto before = *profile;
  if (const auto rejection = edit()) {
    *profile = std::move(before);
    themed_message_box(dialog, rejection->c_str(), title, MB_OK | MB_ICONWARNING);
    return Mutation::rejected;
  }
  const auto validation = layerc::core::validate_automation_profile(
      *profile, document_.profile());
  if (!validation.message.empty()) {
    *profile = std::move(before);
    themed_message_box(dialog,
                to_wide(validation.describe()).c_str(),
                title, MB_OK | MB_ICONWARNING);
    return Mutation::invalid;
  }
  record_change_(DocumentChangeKind::immediate);
  return Mutation::committed;
}

void AutomationEditor::refresh(const HWND dialog) {
  const int preferred = selected_index(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_RESETCONTENT, 0, 0);
  if (document_.automation()) {
    for (const auto &automation : document_.automation()->automations) {
      const auto label = automation_label(automation);
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
           document_.automation()
               ? to_wide(layerc::core::json::format(
                     layerc::core::serialize_automation_profile_json(*document_.automation())))
               : L"{\r\n  \"schemaVersion\": 1,\r\n  \"profileId\": \"\",\r\n"
                 L"  \"continuousInput\": {},\r\n  \"variables\": [],\r\n"
                 L"  \"automations\": []\r\n}\r\n");
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
           to_wide(trace_display_));
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
  const bool selected = rule != nullptr;
  for (const int id :
       {IDC_AUTOMATION_RULE_ID, IDC_AUTOMATION_EVENT, IDC_AUTOMATION_SOURCE,
        IDC_AUTOMATION_PRIORITY, IDC_AUTOMATION_ACTION_KIND,
        IDC_AUTOMATION_TARGET, IDC_AUTOMATION_VALUE, IDC_AUTOMATION_REPEATING,
        IDC_AUTOMATION_APPLY_NODE, IDC_AUTOMATION_DELETE_NODE})
    EnableWindow(GetDlgItem(dialog, id), selected);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ACTION_KIND),
               action != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_TARGET), action != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_VALUE), action != nullptr);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING), action != nullptr);

  update_add_enabled(dialog);
  if (rule == nullptr) {
    const auto *automation = selected_automation(dialog);
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND),
             automation == nullptr
                 ? L"No automation yet — choose a preset above and press New."
             : automation->rules.empty()
                 ? L"No rules yet — choose a rule type and press Add."
                 : L"Select a rule or action in the list.");
    for (const int id :
         {IDC_AUTOMATION_RULE_ID, IDC_AUTOMATION_SOURCE,
          IDC_AUTOMATION_PRIORITY, IDC_AUTOMATION_TARGET, IDC_AUTOMATION_VALUE})
      set_text(GetDlgItem(dialog, id), L"");
    SendDlgItemMessageW(dialog, IDC_AUTOMATION_EVENT, CB_SETCURSEL,
                        static_cast<WPARAM>(-1), 0);
    SendDlgItemMessageW(dialog, IDC_AUTOMATION_ACTION_KIND, CB_SETCURSEL,
                        static_cast<WPARAM>(-1), 0);
    set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING),
                            false);
    return;
  }

  if (action == nullptr)
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND), L"Event rule");
  else
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND),
             L"Action " + std::to_wstring(node_action_ + 1) +
                 L" in selected rule");
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_RULE_ID), to_wide(rule->id));
  const auto event = std::ranges::find(automation_event_kinds, rule->event);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_EVENT, CB_SETCURSEL,
                      event == automation_event_kinds.end()
                          ? static_cast<WPARAM>(-1)
                          : static_cast<WPARAM>(std::distance(
                                automation_event_kinds.begin(), event)),
                      0);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SOURCE),
           to_wide(rule->source_filter));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_PRIORITY),
           std::to_wstring(rule->priority));

  if (action == nullptr) {
    SendDlgItemMessageW(dialog, IDC_AUTOMATION_ACTION_KIND, CB_SETCURSEL,
                        static_cast<WPARAM>(-1), 0);
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET), L"");
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_VALUE), L"");
    set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING),
                            false);
    return;
  }
  const auto kind = std::ranges::find(
      automation_actions, action->kind,
      &layerc::core::AutomationActionSpec::kind);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_ACTION_KIND, CB_SETCURSEL,
                      kind == automation_actions.end()
                          ? static_cast<WPARAM>(-1)
                          : static_cast<WPARAM>(std::distance(
                                automation_actions.begin(), kind)),
                      0);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET), to_wide(action->target));
  std::wstring value_text;
  if (action->value.has_value()) {
    if (action->value->kind == layerc::core::ExpressionKind::literal)
      value_text =
          to_wide(layerc::core::automation_value_json(action->value->literal));
    else
      value_text = kAdvancedExpressionText;
  }
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_VALUE), value_text);
  // Set after set_text: its EN_CHANGE clears the flag.
  advanced_expression_displayed_ =
      action->value.has_value() &&
      action->value->kind != layerc::core::ExpressionKind::literal;
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING),
                          action->repeating);
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
  const auto result = mutate_automation(
      dialog, L"Rule properties", [&]() -> std::optional<std::wstring> {
        auto &rule = *selected_rule(dialog);
        rule.id =
            wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_RULE_ID)));
        const int event_index = static_cast<int>(SendDlgItemMessageW(
            dialog, IDC_AUTOMATION_EVENT, CB_GETCURSEL, 0, 0));
        if (event_index >= 0 && static_cast<std::size_t>(event_index) <
                                    automation_event_kinds.size())
          rule.event =
              automation_event_kinds[static_cast<std::size_t>(event_index)];
        rule.source_filter =
            wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_SOURCE)));
        rule.priority = _wtoi(
            window_text(GetDlgItem(dialog, IDC_AUTOMATION_PRIORITY)).c_str());

        auto *action = selected_action(dialog);
        if (action == nullptr)
          return std::nullopt;
        const int kind_index = static_cast<int>(SendDlgItemMessageW(
            dialog, IDC_AUTOMATION_ACTION_KIND, CB_GETCURSEL, 0, 0));
        if (kind_index >= 0 &&
            static_cast<std::size_t>(kind_index) < automation_actions.size())
          action->kind =
              automation_actions[static_cast<std::size_t>(kind_index)].kind;
        action->target =
            wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET)));
        action->repeating =
            action->kind == layerc::core::AutomationActionKind::create_timer &&
            owner_draw_checkbox_checked(
                GetDlgItem(dialog, IDC_AUTOMATION_REPEATING));
        const auto expected =
            layerc::core::automation_action_value_type(
                *action, *document_.automation());
        if (!expected.has_value() || advanced_expression_displayed_)
          return std::nullopt;
        const auto value_source =
            wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_VALUE)));
        if (value_source.empty())
          return L"This action requires a typed JSON value.";
        const auto parsed =
            layerc::core::parse_automation_value_json(value_source, *expected);
        const auto *value = std::get_if<layerc::core::AutomationValue>(&parsed);
        if (value == nullptr) {
          const auto &error = std::get<layerc::core::AutomationError>(parsed);
          return to_wide(error.describe());
        }
        action->value = literal(*value);
        return std::nullopt;
      });
  // A refused value keeps the typed field text; committed and invalid edits
  // reload the properties from the profile.
  if (result != Mutation::rejected)
    refresh(dialog);
}

void AutomationEditor::delete_node(const HWND dialog) {
  const int automation_index = selected_index(dialog);
  auto *automation = selected_automation(dialog);
  auto *rule = selected_rule(dialog);
  if (automation == nullptr || rule == nullptr)
    return;
  if (selected_action(dialog) != nullptr) {
    rule->actions.erase(rule->actions.begin() + node_action_);
    if (rule->actions.empty())
      automation->rules.erase(automation->rules.begin() + node_rule_);
  } else {
    automation->rules.erase(automation->rules.begin() + node_rule_);
  }
  node_rule_ = -1;
  node_action_ = -1;
  if (automation->rules.empty()) {
    auto &profile = document_.automation();
    profile->automations.erase(profile->automations.begin() + automation_index);
    if (profile->automations.empty() && profile->variables.empty())
      profile.reset();
  }
  document_.mark_automation_edited();
  record_change_(DocumentChangeKind::immediate);
  refresh(dialog);
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
  for (const wchar_t *entry :
       {L"Blank", L"Input starts a cooldown", L"Repeating session timer",
        L"Ready overlay pulse"})
    SendMessageW(presets, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
  SendMessageW(presets, CB_SETCURSEL, 0, 0);

  const HWND views = GetDlgItem(dialog, IDC_AUTOMATION_VIEW);
  for (const wchar_t *entry : {L"Basic", L"Simulator", L"Advanced JSON"})
    SendMessageW(views, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
  SendMessageW(views, CB_SETCURSEL, 0, 0);

  const HWND property_event = GetDlgItem(dialog, IDC_AUTOMATION_EVENT);
  for (const auto kind : automation_event_kinds) {
    const auto label = event_label(kind);
    SendMessageW(property_event, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  const HWND property_action = GetDlgItem(dialog, IDC_AUTOMATION_ACTION_KIND);
  for (const auto &action : automation_actions) {
    const auto label = action_label(action.kind);
    SendMessageW(property_action, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }

  const HWND palette = GetDlgItem(dialog, IDC_AUTOMATION_PALETTE);
  for (const auto &entry : automation_palette)
    SendMessageW(palette, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(entry.label));
  SendMessageW(palette, CB_SETCURSEL, 0, 0);
  const HWND event = GetDlgItem(dialog, IDC_AUTOMATION_SIM_EVENT);
  for (const auto kind : automation_event_kinds) {
    const auto label = event_label(kind);
    SendMessageW(event, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  SendMessageW(event, CB_SETCURSEL, 0, 0);
  const HWND speed = GetDlgItem(dialog, IDC_AUTOMATION_SIM_SPEED);
  for (const wchar_t *entry : {L"0.25x", L"1x", L"4x"})
    SendMessageW(speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
  SendMessageW(speed, CB_SETCURSEL, 1, 0);
  update_view(dialog);
  refresh(dialog);
}

AutomationEditor::AutomationEditor(ProfileDocument &document,
                                   LiveEditSession &live_edit,
                                   const EditorShell &shell,
                                   const DialogTheme &theme,
                                   ChangeRecorder record_change) noexcept
    : document_(document), live_edit_(live_edit), shell_(shell), theme_(theme),
      record_change_(std::move(record_change)) {}
AutomationEditor::~AutomationEditor() = default;

void AutomationEditor::open() {
  DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_AUTOMATION), shell_.window,
      &dispatch_member_dialog<AutomationEditor, &AutomationEditor::initialize,
                              &AutomationEditor::handle_message>,
      reinterpret_cast<LPARAM>(this));
}

void AutomationEditor::add_basic(const HWND dialog) {
  const int preset =
      std::max(0, static_cast<int>(SendDlgItemMessageW(
                      dialog, IDC_AUTOMATION_PRESETS, CB_GETCURSEL, 0, 0)));
  if (preset != 0 && document_.profile().cooldowns.empty()) {
    themed_message_box(dialog,
                L"Add a cooldown before creating this automation preset.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (preset == 3 && document_.profile().panels.empty()) {
    themed_message_box(dialog, L"The overlay preset requires at least one panel.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!document_.automation()) {
    layerc::core::AutomationProfile created;
    created.profile_id = document_.profile().id;
    document_.automation() = std::move(created);
  }

  const auto id = numbered_unique_id(
      "automation", document_.automation()->automations);
  const auto cooldown_id = document_.profile().cooldowns.empty()
                               ? std::string{}
                               : document_.profile().cooldowns.front().id;
  const auto panel_id = document_.profile().panels.empty()
                            ? std::string{}
                            : document_.profile().panels.front().id;
  document_.automation()->automations.push_back(make_automation_preset(
      static_cast<AutomationPreset>(preset), id, cooldown_id, panel_id));
  document_.mark_automation_edited();
  record_change_(DocumentChangeKind::immediate);
  refresh(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_SETCURSEL,
                      document_.automation()->automations.size() - 1U, 0);
  node_rule_ = document_.automation()->automations.back().rules.empty()
                    ? -1
                    : 0;
  node_action_ = -1;
  refresh(dialog);
}
// Adding is offered only when it can succeed: an action needs a selected
// rule, some entries need a cooldown or an overlay bar, and non-blank
// presets need a cooldown.
void AutomationEditor::update_add_enabled(const HWND dialog) {
  const auto &profile = document_.profile();
  const auto *entry = automation_palette_entry(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PALETTE, CB_GETCURSEL, 0, 0));
  bool can_add = entry != nullptr;
  if (entry != nullptr) {
    if (entry->requires_cooldown && profile.cooldowns.empty())
      can_add = false;
    if (entry->requires_panel && profile.panels.empty())
      can_add = false;
    if (entry->target == AutomationPaletteTarget::action &&
        selected_rule(dialog) == nullptr)
      can_add = false;
  }
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ADD_NODE), can_add);
  const LRESULT preset =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PRESETS, CB_GETCURSEL, 0, 0);
  EnableWindow(GetDlgItem(dialog, IDC_AUTOMATION_ADD_PRESET),
               preset <= 0 || (!profile.cooldowns.empty() &&
                               (preset != 3 || !profile.panels.empty())));
}

void AutomationEditor::add_palette_node(const HWND dialog) {
  const int selected = selected_index(dialog);
  if (selected < 0) {
    add_basic(dialog);
    return;
  }
  const auto *entry = automation_palette_entry(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PALETTE, CB_GETCURSEL, 0, 0));
  if (entry == nullptr)
    return;
  const auto &profile = document_.profile();
  if (entry->requires_cooldown && profile.cooldowns.empty()) {
    themed_message_box(dialog, L"This rule type needs at least one cooldown.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const bool adds_action = entry->target == AutomationPaletteTarget::action;
  if (adds_action && selected_rule(dialog) == nullptr) {
    themed_message_box(dialog, L"Select a rule before adding an action.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (entry->requires_panel && profile.panels.empty()) {
    themed_message_box(dialog, L"Overlay actions need at least one overlay bar.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto cooldown_id =
      profile.cooldowns.empty() ? std::string{} : profile.cooldowns.front().id;
  const auto panel_id =
      profile.panels.empty() ? std::string{} : profile.panels.front().id;
  const auto result = mutate_automation(
      dialog, L"Could not add the rule or action",
      [&]() -> std::optional<std::wstring> {
        auto &automation = *selected_automation(dialog);
        if (entry->target == AutomationPaletteTarget::rule) {
          automation.rules.push_back(make_palette_rule(
              entry->node, numbered_unique_id("rule", automation.rules),
              cooldown_id));
        } else if (adds_action) {
          auto &rule = *selected_rule(dialog);
          rule.actions.push_back(make_palette_action(
              entry->node, rule.actions.size(), cooldown_id, panel_id));
        } else {
          auto &variables = document_.automation()->variables;
          variables.push_back(
              make_palette_variable(numbered_unique_id("flag", variables)));
        }
        return std::nullopt;
      });
  if (result != Mutation::committed)
    return;
  if (entry->target == AutomationPaletteTarget::rule) {
    if (const auto *automation = selected_automation(dialog)) {
      node_rule_ = static_cast<int>(automation->rules.size()) - 1;
      node_action_ = -1;
    }
  } else if (adds_action) {
    if (const auto *rule = selected_rule(dialog))
      node_action_ = static_cast<int>(rule->actions.size()) - 1;
  }
  refresh(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, CB_SETCURSEL, selected, 0);
  refresh(dialog);
}

void AutomationEditor::remove_selected(const HWND dialog) {
  const int selected = selected_index(dialog);
  if (selected < 0)
    return;
  auto &profile = document_.automation();
  profile->automations.erase(profile->automations.begin() + selected);
  node_rule_ = -1;
  node_action_ = -1;
  if (profile->automations.empty() && profile->variables.empty())
    profile.reset();
  document_.mark_automation_edited();
  record_change_(DocumentChangeKind::immediate);
  refresh(dialog);
}

bool AutomationEditor::apply_json(const HWND dialog) {
  const auto source =
      wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON)));
  const auto parsed =
      layerc::core::parse_automation_profile_json(source, document_.profile());
  const auto *profile = std::get_if<layerc::core::AutomationProfile>(&parsed);
  if (profile == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    const auto presentation = present_json_error(source, error);
    themed_message_box(dialog, presentation.message.c_str(),
                L"Invalid automation graph", MB_OK | MB_ICONWARNING);
    select_json_error(dialog, presentation);
    return false;
  }
  document_.automation() = *profile;
  node_rule_ = -1;
  node_action_ = -1;
  document_.mark_automation_edited();
  record_change_(DocumentChangeKind::immediate);
  refresh(dialog);
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
      if (auto *selected = selected_automation(dialog)) {
        selected->enabled = owner_draw_checkbox_checked(
            GetDlgItem(dialog, IDC_AUTOMATION_ENABLED));
        record_change_(DocumentChangeKind::immediate);
        refresh(dialog);
      }
    }
    return true;
  case IDC_AUTOMATION_NAME:
    if (notification == EN_KILLFOCUS && document_.automation()) {
      if (auto *selected = selected_automation(dialog)) {
        selected->name =
            wide_to_utf8(window_text(GetDlgItem(dialog, IDC_AUTOMATION_NAME)));
        record_change_(DocumentChangeKind::text);
      }
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
    (void)chrome_.on_dpi_changed(HIWORD(wparam), lparam);
    layout(window);
    return TRUE;
  }
  if (message == WM_GETMINMAXINFO) {
    chrome_.on_minimum_size(lparam, 780, 520);
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
