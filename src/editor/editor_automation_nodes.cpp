#include "automation_editor.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include <string>

#include <windows.h>

namespace layerc::editor {
std::wstring event_label(const layerc::core::AutomationEventKind kind) {
  using layerc::core::AutomationEventKind;
  switch (kind) {
  case AutomationEventKind::input_pressed:
    return L"Input pressed";
  case AutomationEventKind::input_released:
    return L"Input released";
  case AutomationEventKind::input_value_changed:
    return L"Input value";
  case AutomationEventKind::visual_observation:
    return L"Visual observation";
  case AutomationEventKind::cooldown_triggered:
    return L"Cooldown triggered";
  case AutomationEventKind::cooldown_ready:
    return L"Cooldown ready";
  case AutomationEventKind::group_triggered:
    return L"Group triggered";
  case AutomationEventKind::group_ready:
    return L"Group ready";
  case AutomationEventKind::session_started:
    return L"Session started";
  case AutomationEventKind::session_stopping:
    return L"Session stopping";
  case AutomationEventKind::variable_changed:
    return L"Variable changed";
  case AutomationEventKind::timer_fired:
    return L"Timer fired";
  }
  return L"Session started";
}

std::wstring action_label(const layerc::core::AutomationActionKind kind) {
  const auto *spec = layerc::core::automation_action_spec(kind);
  return spec == nullptr ? L"Invalid action" : to_wide(spec->display_name);
}

void AutomationEditor::refresh_nodes(const HWND dialog) {
  const auto *definition = selected_automation(dialog);
  if (selected_rule(dialog) == nullptr) {
    node_rule_ = -1;
    node_action_ = -1;
  } else if (node_action_ >= 0 && selected_action(dialog) == nullptr) {
    node_action_ = -1;
  }

  const HWND tree = GetDlgItem(dialog, IDC_AUTOMATION_NODES);
  SendMessageW(tree, LB_RESETCONTENT, 0, 0);
  int selected_item = -1;
  if (definition != nullptr) {
    for (std::size_t rule_index{}; rule_index < definition->rules.size();
         ++rule_index) {
      const auto &rule = definition->rules[rule_index];
      auto label = L"Rule: " + to_wide(rule.id) + L" — " +
                   event_label(rule.event);
      if (!rule.source_filter.empty())
        label += L" [" + to_wide(rule.source_filter) + L"]";
      const int item = static_cast<int>(SendMessageW(
          tree, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
      SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(item),
                   automation_tree_item_data(static_cast<int>(rule_index), -1));
      if (node_rule_ == static_cast<int>(rule_index) && node_action_ < 0)
        selected_item = item;

      for (std::size_t action_index{}; action_index < rule.actions.size();
           ++action_index) {
        const auto &action = rule.actions[action_index];
        auto action_text = L"    Action " + std::to_wstring(action_index + 1U) +
                           L": " + action_label(action.kind);
        if (!action.target.empty())
          action_text += L" — " + to_wide(action.target);
        const int action_item = static_cast<int>(SendMessageW(
            tree, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(action_text.c_str())));
        SendMessageW(
            tree, LB_SETITEMDATA, static_cast<WPARAM>(action_item),
            automation_tree_item_data(static_cast<int>(rule_index),
                                      static_cast<int>(action_index)));
        if (node_rule_ == static_cast<int>(rule_index) &&
            node_action_ == static_cast<int>(action_index))
          selected_item = action_item;
      }
    }
    if (definition->rules.empty()) {
      const int item = static_cast<int>(SendMessageW(
          tree, LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(L"No rules. Choose a rule type and Add.")));
      SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(item), LB_ERR);
    }
  }
  if (selected_item >= 0)
    SendMessageW(tree, LB_SETCURSEL, static_cast<WPARAM>(selected_item), 0);

  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_RESETCONTENT, 0, 0);
  std::wstring validation = L"No automations yet.";
  validation_has_error_ = false;
  if (document_.automation()) {
    const auto error = layerc::core::validate_automation_profile(
        *document_.automation(), document_.profile());
    validation_has_error_ = !error.message.empty();
    validation = validation_has_error_
                     ? to_wide(error.path + ": " + error.message)
                     : L"Valid — no errors";
  }
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_ADDSTRING, 0,
                      reinterpret_cast<LPARAM>(validation.c_str()));
  // Auto-expand only while broken; a fixed profile collapses back to one row.
  validation_expanded_ = validation_has_error_;

  refresh_node_properties(dialog);
  layout(dialog);
}
} // namespace layerc::editor
