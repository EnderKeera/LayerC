#include "automation_editor.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include <string>

#include <windows.h>

namespace layerc::editor {
void AutomationEditor::refresh_nodes(const HWND dialog) {
  const auto selection = clamp_selection(
      document_.automation(), {selected_index(dialog), node_rule_, node_action_});
  node_rule_ = selection.rule;
  node_action_ = selection.action;

  const HWND tree = GetDlgItem(dialog, IDC_AUTOMATION_NODES);
  SendMessageW(tree, LB_RESETCONTENT, 0, 0);
  int selected_item = -1;
  if (const auto *definition = selected_automation(dialog)) {
    const auto rows = automation_node_rows(*definition);
    for (const auto &row : rows) {
      const auto label = to_wide(row.label);
      const int item = static_cast<int>(SendMessageW(
          tree, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
      SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(item),
                   automation_tree_item_data(row.rule, row.action));
      if (node_rule_ == row.rule && node_action_ == row.action)
        selected_item = item;
    }
    if (rows.empty()) {
      const int item = static_cast<int>(SendMessageW(
          tree, LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(L"No rules. Choose a rule type and Add.")));
      SendMessageW(tree, LB_SETITEMDATA, static_cast<WPARAM>(item), LB_ERR);
    }
  }
  if (selected_item >= 0)
    SendMessageW(tree, LB_SETCURSEL, static_cast<WPARAM>(selected_item), 0);

  const auto validation = to_wide(
      automation_validation_summary(document_.automation(), document_.profile()));
  validation_has_error_ = document_.automation() &&
      !validate_automation_graph(*document_.automation(), document_.profile()).empty();
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_RESETCONTENT, 0, 0);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_ADDSTRING, 0,
                      reinterpret_cast<LPARAM>(validation.c_str()));
  // Auto-expand only while broken; a fixed profile collapses back to one row.
  validation_expanded_ = validation_has_error_;
  refresh_node_properties(dialog);
  layout(dialog);
}
} // namespace layerc::editor
