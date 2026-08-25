#include "automation_editor.hpp"
#include "editor_automation_internal.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"

#include <algorithm>
#include <ranges>

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
  using layerc::core::AutomationActionKind;
  switch (kind) {
  case AutomationActionKind::set_variable:
    return L"Set variable";
  case AutomationActionKind::increment_variable:
    return L"Increment variable";
  case AutomationActionKind::append_list:
    return L"Append list";
  case AutomationActionKind::set_map_entry:
    return L"Set map entry";
  case AutomationActionKind::erase_map_entry:
    return L"Erase map entry";
  case AutomationActionKind::trigger_cooldown:
    return L"Trigger cooldown";
  case AutomationActionKind::reset_cooldown:
    return L"Reset cooldown";
  case AutomationActionKind::reduce_cooldown_next:
    return L"Reduce next recovery";
  case AutomationActionKind::reduce_cooldown_all:
    return L"Reduce all recoveries";
  case AutomationActionKind::refund_cooldown:
    return L"Refund cooldown";
  case AutomationActionKind::trigger_group:
    return L"Trigger group";
  case AutomationActionKind::reset_group:
    return L"Reset group";
  case AutomationActionKind::create_timer:
    return L"Create timer";
  case AutomationActionKind::cancel_timer:
    return L"Cancel timer";
  case AutomationActionKind::set_overlay_override:
    return L"Set overlay override";
  case AutomationActionKind::clear_overlay_override:
    return L"Clear overlay override";
  }
  return L"Internal action";
}

std::vector<AutomationNodeRect>
automation_node_rects(const RECT &canvas,
                      const layerc::core::AutomationDefinition &automation,
                      const int zoom) {
  const int scale = std::clamp(zoom, 50, 200);
  const auto scaled = [scale](const int value) {
    return std::max(1, MulDiv(value, scale, 100));
  };
  const int margin = scaled(10);
  const int node_width = scaled(112);
  const int event_height = scaled(30);
  const int action_height = scaled(24);
  const int action_gap = scaled(4);
  const int row_gap = scaled(10);
  const int event_left = canvas.left + margin;
  const int action_left = canvas.left + margin + scaled(154);
  int y = canvas.top + margin;
  std::vector<AutomationNodeRect> result;
  for (std::size_t rule_index{}; rule_index < automation.rules.size();
       ++rule_index) {
    const auto &rule = automation.rules[rule_index];
    const int action_stack = rule.actions.empty()
                                 ? 0
                                 : static_cast<int>(rule.actions.size()) *
                                           (action_height + action_gap) -
                                       action_gap;
    const int row_height = std::max(event_height, action_stack);
    if (y + std::min(row_height, event_height) > canvas.bottom - margin)
      break;
    result.push_back(
        {.rule = static_cast<int>(rule_index),
         .action = -1,
         .rect = {event_left, y, event_left + node_width, y + event_height}});
    for (std::size_t action_index{}; action_index < rule.actions.size();
         ++action_index) {
      const int action_y =
          y + static_cast<int>(action_index) * (action_height + action_gap);
      if (action_y + action_height > canvas.bottom - margin)
        break;
      result.push_back(
          {.rule = static_cast<int>(rule_index),
           .action = static_cast<int>(action_index),
           .rect = {action_left, action_y, action_left + node_width,
                    action_y + action_height}});
    }
    y += row_height + row_gap;
  }
  return result;
}

void AutomationEditor::refresh_graph(const HWND dialog) {
  const int selected = selected_index(dialog);
  const auto *definition =
      owner_.automation_profile_ && selected >= 0 &&
              static_cast<std::size_t>(selected) <
                  owner_.automation_profile_->automations.size()
          ? &owner_.automation_profile_
                 ->automations[static_cast<std::size_t>(selected)]
          : nullptr;
  std::wstring properties =
      definition == nullptr
          ? L"Select an automation or add a Basic preset."
          : L"ID: " + to_wide(definition->id) + L"    Rules: " +
                std::to_wstring(definition->rules.size()) + L"\r\n";
  if (definition != nullptr) {
    for (const auto &rule : definition->rules) {
      properties += event_label(rule.event);
      if (!rule.source_filter.empty())
        properties += L" [" + to_wide(rule.source_filter) + L"]";
      properties += L" -> " + std::to_wstring(rule.actions.size()) +
                    L" action(s); priority " + std::to_wstring(rule.priority) +
                    L"\r\n";
    }
  }
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_PROPERTIES), properties);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_RESETCONTENT, 0, 0);
  std::wstring line = L"No automation module";
  if (owner_.automation_profile_) {
    const auto error = layerc::core::validate_automation_profile(
        *owner_.automation_profile_, owner_.profile_);
    line = error.message.empty() ? L"Valid - no errors"
                                 : to_wide(error.path + ": " + error.message);
  }
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_ADDSTRING, 0,
                      reinterpret_cast<LPARAM>(line.c_str()));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_ZOOM_LABEL),
           std::to_wstring(graph_zoom_) + L"%");
  if (definition == nullptr || graph_rule_ < 0 ||
      static_cast<std::size_t>(graph_rule_) >= definition->rules.size()) {
    graph_rule_ = -1;
    graph_action_ = -1;
  } else if (graph_action_ >= 0 &&
             static_cast<std::size_t>(graph_action_) >=
                 definition->rules[static_cast<std::size_t>(graph_rule_)]
                     .actions.size()) {
    graph_action_ = -1;
  }
  refresh_node_properties(dialog);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_CANVAS), nullptr, TRUE);
}

void AutomationEditor::draw_canvas(const DRAWITEMSTRUCT &item) const {
  const auto canvas_colors =
      owner_.colors(layerc::platform::EditorControlRole::preview);
  const auto list_colors =
      owner_.colors(layerc::platform::EditorControlRole::list);
  const HBRUSH background = CreateSolidBrush(canvas_colors.fill);
  FillRect(item.hDC, &item.rcItem, background);
  DeleteObject(background);

  const int selected = selected_index(GetParent(item.hwndItem));
  if (!owner_.automation_profile_ || selected < 0 ||
      static_cast<std::size_t>(selected) >=
          owner_.automation_profile_->automations.size()) {
    RECT message = item.rcItem;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, canvas_colors.disabled);
    const HGDIOBJ previous_font = SelectObject(item.hDC, owner_.font_);
    DrawTextW(item.hDC, L"Select an automation to inspect its graph.", -1,
              &message, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, previous_font);
    return;
  }

  const auto &automation =
      owner_.automation_profile_
          ->automations[static_cast<std::size_t>(selected)];
  const int scale = std::clamp(graph_zoom_, 50, 200);
  const auto scaled = [scale](const int value) {
    return std::max(1, MulDiv(value, scale, 100));
  };
  const auto nodes =
      automation_node_rects(item.rcItem, automation, graph_zoom_);
  const HPEN edge_pen =
      CreatePen(PS_SOLID, std::max(1, scaled(1)), canvas_colors.border);
  const HGDIOBJ previous_pen = SelectObject(item.hDC, edge_pen);
  const HGDIOBJ previous_font = SelectObject(item.hDC, owner_.font_);
  SetBkMode(item.hDC, TRANSPARENT);

  const auto draw_node = [&](const AutomationNodeRect &node,
                             const std::wstring &label) {
    const bool action = node.action >= 0;
    const bool selected_node =
        node.rule == graph_rule_ && node.action == graph_action_;
    const COLORREF fill =
        selected_node ? list_colors.hover
                      : (action ? list_colors.selection : list_colors.fill);
    const HBRUSH node_fill = CreateSolidBrush(fill);
    FillRect(item.hDC, &node.rect, node_fill);
    DeleteObject(node_fill);
    const HBRUSH node_border = CreateSolidBrush(
        selected_node ? canvas_colors.selection : list_colors.border);
    FrameRect(item.hDC, &node.rect, node_border);
    DeleteObject(node_border);
    RECT text = node.rect;
    text.left += scaled(5);
    text.right -= scaled(5);
    SetTextColor(item.hDC, list_colors.text);
    DrawTextW(item.hDC, label.c_str(), static_cast<int>(label.size()), &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  };

  for (const auto &node : nodes) {
    const auto &rule = automation.rules[static_cast<std::size_t>(node.rule)];
    if (node.action < 0) {
      auto label = event_label(rule.event);
      if (!rule.source_filter.empty())
        label += L": " + to_wide(rule.source_filter);
      draw_node(node, label);
      continue;
    }
    const auto event_node =
        std::ranges::find_if(nodes, [&](const AutomationNodeRect &candidate) {
          return candidate.rule == node.rule && candidate.action < 0;
        });
    if (event_node != nodes.end()) {
      MoveToEx(item.hDC, event_node->rect.right,
               (event_node->rect.top + event_node->rect.bottom) / 2, nullptr);
      LineTo(item.hDC, node.rect.left, (node.rect.top + node.rect.bottom) / 2);
    }
    const auto &action = rule.actions[static_cast<std::size_t>(node.action)];
    auto label = action_label(action.kind);
    if (!action.target.empty())
      label += L": " + to_wide(action.target);
    draw_node(node, label);
  }
  if (automation.rules.empty()) {
    RECT empty = item.rcItem;
    SetTextColor(item.hDC, canvas_colors.disabled);
    DrawTextW(item.hDC, L"No rules. Add an event node from the palette.", -1,
              &empty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  SelectObject(item.hDC, previous_font);
  SelectObject(item.hDC, previous_pen);
  DeleteObject(edge_pen);
}
} // namespace layerc::editor
