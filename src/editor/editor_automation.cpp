#include "editor_resource.h"
#include "automation_editor.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_window.hpp"

#include "layerc/core/profile.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <variant>

#include <commdlg.h>
#include <windows.h>

namespace layerc::editor {
namespace {
using namespace std::chrono_literals;

std::string
unique_automation_id(const layerc::core::AutomationProfile &profile) {
  for (std::size_t suffix = profile.automations.size() + 1;; ++suffix) {
    const auto id = "automation-" + std::to_string(suffix);
    if (std::ranges::none_of(profile.automations,
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

constexpr UINT_PTR automation_simulator_timer = 94'001;

std::optional<std::filesystem::path> choose_trace_path(const HWND owner,
                                                       const bool save) {
  std::array<wchar_t, 32'768> buffer{};
  if (save)
    wcscpy_s(buffer.data(), buffer.size(), L"automation.layerctrace");
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter =
      L"LayerC traces (*.layerctrace)\0*.layerctrace\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"layerctrace";
  dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL selected =
      save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (selected == FALSE)
    return std::nullopt;
  auto path = std::filesystem::path(buffer.data());
  if (save && _wcsicmp(path.extension().c_str(), L".layerctrace") != 0)
    path.replace_extension(L".layerctrace");
  return path;
}

std::optional<std::string>
read_bounded_trace(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > layerc::core::max_automation_trace_bytes)
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  return input.gcount() == static_cast<std::streamsize>(result.size())
             ? std::optional(std::move(result))
             : std::nullopt;
}

std::string
trace_from_events(const std::span<const layerc::core::AutomationEvent> events) {
  std::vector<layerc::core::AutomationTraceEntry> entries;
  entries.reserve(events.size());
  for (std::size_t index{}; index < events.size(); ++index) {
    layerc::core::AutomationTraceEntry entry;
    entry.sequence = index + 1U;
    entry.timestamp = std::chrono::duration_cast<layerc::core::Milliseconds>(
        events[index].timestamp.time_since_epoch());
    entry.phase = "source";
    entry.detail = "recorded event";
    entry.source_event = events[index];
    entries.push_back(std::move(entry));
  }
  return layerc::core::export_layerc_trace(entries);
}

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

layerc::core::AutomationExpression
literal(layerc::core::AutomationValue value) {
  layerc::core::AutomationExpression expression;
  expression.kind = layerc::core::ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

// One list with the core wire format: combo order, label lookup, and the
// simulator all index the same canonical ordering.
constexpr auto &automation_event_kinds =
    layerc::core::automation_event_kinds;

constexpr std::array automation_action_kinds{
    layerc::core::AutomationActionKind::set_variable,
    layerc::core::AutomationActionKind::increment_variable,
    layerc::core::AutomationActionKind::append_list,
    layerc::core::AutomationActionKind::set_map_entry,
    layerc::core::AutomationActionKind::erase_map_entry,
    layerc::core::AutomationActionKind::trigger_cooldown,
    layerc::core::AutomationActionKind::reset_cooldown,
    layerc::core::AutomationActionKind::reduce_cooldown_next,
    layerc::core::AutomationActionKind::reduce_cooldown_all,
    layerc::core::AutomationActionKind::refund_cooldown,
    layerc::core::AutomationActionKind::trigger_group,
    layerc::core::AutomationActionKind::reset_group,
    layerc::core::AutomationActionKind::create_timer,
    layerc::core::AutomationActionKind::cancel_timer,
    layerc::core::AutomationActionKind::set_overlay_override,
    layerc::core::AutomationActionKind::clear_overlay_override};

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

struct AutomationNodeRect {
  int rule{};
  int action{-1};
  RECT rect{};
};

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

std::optional<layerc::core::AutomationValueType>
action_value_type(const layerc::core::AutomationAction &action,
                  const layerc::core::AutomationProfile &profile) {
  using layerc::core::AutomationActionKind;
  using layerc::core::AutomationValueType;
  if (action.kind == AutomationActionKind::set_variable ||
      action.kind == AutomationActionKind::increment_variable) {
    const auto variable =
        std::ranges::find(profile.variables, action.target,
                          &layerc::core::AutomationVariable::id);
    return variable == profile.variables.end() ? std::nullopt
                                               : std::optional(variable->type);
  }
  if (action.kind == AutomationActionKind::erase_map_entry)
    return AutomationValueType::string;
  if (action.kind == AutomationActionKind::reduce_cooldown_next ||
      action.kind == AutomationActionKind::reduce_cooldown_all ||
      action.kind == AutomationActionKind::create_timer)
    return AutomationValueType::duration;
  if (action.kind != AutomationActionKind::set_overlay_override)
    return std::nullopt;
  const auto *spec = layerc::core::find_overlay_property(action.property);
  return spec == nullptr ? std::optional(AutomationValueType::integer)
                         : std::optional(spec->type);
}

} // namespace

EditorWindow::EditorWindow()
    : automation_editor_(std::make_unique<AutomationEditor>(*this)) {}

EditorWindow::~EditorWindow() = default;

void EditorWindow::open_automation_editor() { automation_editor_->open(); }

void EditorWindow::load_current_automation_profile() {
  automation_profile_.reset();
  automation_sidecar_state_ = layerc::platform::AutomationSidecarState::absent;
  automation_sidecar_error_.clear();
  if (current_path_.empty())
    return;

  auto loaded =
      layerc::platform::load_automation_sidecar(current_path_, profile_);
  automation_sidecar_state_ = loaded.state;
  automation_sidecar_error_ = loaded.error.message;
  if (loaded.profile)
    automation_profile_ = std::move(loaded.profile);
}

int AutomationEditor::selected_index(const HWND dialog) const {
  const auto selected =
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_GETCURSEL, 0, 0);
  return selected == LB_ERR ? static_cast<WPARAM>(-1)
                            : static_cast<int>(selected);
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
      static_cast<std::size_t>(graph_rule_) >=
          definition->rules.size()) {
    graph_rule_ = -1;
    graph_action_ = -1;
  } else if (graph_action_ >= 0 &&
             static_cast<std::size_t>(graph_action_) >=
                 definition
                     ->rules[static_cast<std::size_t>(graph_rule_)]
                     .actions.size()) {
    graph_action_ = -1;
  }
  refresh_node_properties(dialog);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_CANVAS), nullptr, TRUE);
}

void AutomationEditor::refresh(const HWND dialog) {
  const int preferred = selected_index(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_RESETCONTENT, 0, 0);
  if (owner_.automation_profile_) {
    for (const auto &automation : owner_.automation_profile_->automations) {
      const auto label = automation_label(automation);
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_ADDSTRING, 0,
                          reinterpret_cast<LPARAM>(label.c_str()));
    }
  }

  const int count =
      owner_.automation_profile_
          ? static_cast<int>(owner_.automation_profile_->automations.size())
          : 0;
  const int selected = count == 0 ? static_cast<WPARAM>(-1)
                                  : std::clamp(preferred, 0, count - 1);
  if (selected >= 0)
    SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_SETCURSEL, selected, 0);
  const auto *definition =
      selected >= 0 ? &owner_.automation_profile_
                           ->automations[static_cast<std::size_t>(selected)]
                    : nullptr;
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_ENABLED),
                          definition != nullptr && definition->enabled);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_NAME),
           definition == nullptr ? L"" : to_wide(definition->name));
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON),
           owner_.automation_profile_
               ? to_wide(layerc::core::serialize_automation_profile_json(
                     *owner_.automation_profile_))
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
  refresh_graph(dialog);
}

void AutomationEditor::refresh_node_properties(const HWND dialog) {
  const int automation_index = selected_index(dialog);
  const auto *automation =
      owner_.automation_profile_ && automation_index >= 0 &&
              static_cast<std::size_t>(automation_index) <
                  owner_.automation_profile_->automations.size()
          ? &owner_.automation_profile_
                 ->automations[static_cast<std::size_t>(automation_index)]
          : nullptr;
  const auto *rule =
      automation != nullptr && graph_rule_ >= 0 &&
              static_cast<std::size_t>(graph_rule_) <
                  automation->rules.size()
          ? &automation->rules[static_cast<std::size_t>(graph_rule_)]
          : nullptr;
  const auto *action =
      rule != nullptr && graph_action_ >= 0 &&
              static_cast<std::size_t>(graph_action_) <
                  rule->actions.size()
          ? &rule->actions[static_cast<std::size_t>(graph_action_)]
          : nullptr;
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

  if (rule == nullptr) {
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND),
             L"Select a node on the canvas.");
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

  set_text(GetDlgItem(dialog, IDC_AUTOMATION_NODE_KIND),
           action == nullptr
               ? L"Event rule"
               : L"Action " + std::to_wstring(graph_action_ + 1) +
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
  const auto kind = std::ranges::find(automation_action_kinds, action->kind);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_ACTION_KIND, CB_SETCURSEL,
                      kind == automation_action_kinds.end()
                          ? static_cast<WPARAM>(-1)
                          : static_cast<WPARAM>(std::distance(
                                automation_action_kinds.begin(), kind)),
                      0);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET), to_wide(action->target));
  set_text(
      GetDlgItem(dialog, IDC_AUTOMATION_VALUE),
      action->value.kind == layerc::core::ExpressionKind::literal
          ? to_wide(layerc::core::automation_value_json(action->value.literal))
          : L"<Advanced expression>");
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING),
                          action->repeating);
  InvalidateRect(GetDlgItem(dialog, IDC_AUTOMATION_REPEATING), nullptr, TRUE);
}

void AutomationEditor::select_canvas_node(const HWND dialog) {
  const int automation_index = selected_index(dialog);
  if (!owner_.automation_profile_ || automation_index < 0 ||
      static_cast<std::size_t>(automation_index) >=
          owner_.automation_profile_->automations.size())
    return;
  const HWND canvas = GetDlgItem(dialog, IDC_AUTOMATION_CANVAS);
  POINT point{};
  GetCursorPos(&point);
  ScreenToClient(canvas, &point);
  RECT client{};
  GetClientRect(canvas, &client);
  graph_rule_ = -1;
  graph_action_ = -1;
  for (const auto &node : automation_node_rects(
           client,
           owner_.automation_profile_
               ->automations[static_cast<std::size_t>(automation_index)],
           graph_zoom_)) {
    if (PtInRect(&node.rect, point) == FALSE)
      continue;
    graph_rule_ = node.rule;
    graph_action_ = node.action;
    break;
  }
  refresh_graph(dialog);
}

void AutomationEditor::apply_node_properties(const HWND dialog) {
  const int automation_index = selected_index(dialog);
  if (!owner_.automation_profile_ || automation_index < 0 ||
      graph_rule_ < 0 ||
      static_cast<std::size_t>(automation_index) >=
          owner_.automation_profile_->automations.size())
    return;
  auto before = *owner_.automation_profile_;
  auto &automation =
      owner_.automation_profile_
          ->automations[static_cast<std::size_t>(automation_index)];
  if (static_cast<std::size_t>(graph_rule_) >=
      automation.rules.size())
    return;
  auto &rule =
      automation.rules[static_cast<std::size_t>(graph_rule_)];
  rule.id = to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_RULE_ID)));
  const int event_index = static_cast<int>(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_EVENT, CB_GETCURSEL, 0, 0));
  if (event_index >= 0 &&
      static_cast<std::size_t>(event_index) < automation_event_kinds.size())
    rule.event = automation_event_kinds[static_cast<std::size_t>(event_index)];
  rule.source_filter =
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_SOURCE)));
  rule.priority =
      _wtoi(control_text(GetDlgItem(dialog, IDC_AUTOMATION_PRIORITY)).c_str());

  if (graph_action_ >= 0 &&
      static_cast<std::size_t>(graph_action_) <
          rule.actions.size()) {
    auto &action =
        rule.actions[static_cast<std::size_t>(graph_action_)];
    const int kind_index = static_cast<int>(SendDlgItemMessageW(
        dialog, IDC_AUTOMATION_ACTION_KIND, CB_GETCURSEL, 0, 0));
    if (kind_index >= 0 &&
        static_cast<std::size_t>(kind_index) < automation_action_kinds.size())
      action.kind =
          automation_action_kinds[static_cast<std::size_t>(kind_index)];
    action.target =
        to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_TARGET)));
    action.repeating =
        action.kind == layerc::core::AutomationActionKind::create_timer &&
        owner_draw_checkbox_checked(
            GetDlgItem(dialog, IDC_AUTOMATION_REPEATING));
    const auto expected = action_value_type(action, *owner_.automation_profile_);
    const auto value_source =
        to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_VALUE)));
    if (expected.has_value() && value_source != "<Advanced expression>") {
      if (value_source.empty()) {
        *owner_.automation_profile_ = std::move(before);
        MessageBoxW(dialog, L"This action requires a typed JSON value.",
                    L"Node properties", MB_OK | MB_ICONWARNING);
        return;
      }
      const auto parsed =
          layerc::core::parse_automation_value_json(value_source, *expected);
      const auto *value = std::get_if<layerc::core::AutomationValue>(&parsed);
      if (value == nullptr) {
        const auto &error = std::get<layerc::core::AutomationError>(parsed);
        *owner_.automation_profile_ = std::move(before);
        MessageBoxW(dialog, to_wide(error.path + ": " + error.message).c_str(),
                    L"Node properties", MB_OK | MB_ICONWARNING);
        return;
      }
      action.value = literal(*value);
    }
  }
  const auto validation =
      layerc::core::validate_automation_profile(*owner_.automation_profile_, owner_.profile_);
  if (!validation.message.empty()) {
    *owner_.automation_profile_ = std::move(before);
    MessageBoxW(dialog,
                to_wide(validation.path + ": " + validation.message).c_str(),
                L"Node properties", MB_OK | MB_ICONWARNING);
    refresh(dialog);
    return;
  }
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
}

void AutomationEditor::delete_canvas_node(const HWND dialog) {
  const int automation_index = selected_index(dialog);
  if (!owner_.automation_profile_ || automation_index < 0 ||
      graph_rule_ < 0 ||
      static_cast<std::size_t>(automation_index) >=
          owner_.automation_profile_->automations.size())
    return;
  auto &automation =
      owner_.automation_profile_
          ->automations[static_cast<std::size_t>(automation_index)];
  if (static_cast<std::size_t>(graph_rule_) >=
      automation.rules.size())
    return;
  auto &rule =
      automation.rules[static_cast<std::size_t>(graph_rule_)];
  if (graph_action_ >= 0 &&
      static_cast<std::size_t>(graph_action_) <
          rule.actions.size()) {
    rule.actions.erase(rule.actions.begin() + graph_action_);
    if (rule.actions.empty())
      automation.rules.erase(automation.rules.begin() + graph_rule_);
  } else {
    automation.rules.erase(automation.rules.begin() + graph_rule_);
  }
  graph_rule_ = -1;
  graph_action_ = -1;
  if (automation.rules.empty()) {
    owner_.automation_profile_->automations.erase(
        owner_.automation_profile_->automations.begin() + automation_index);
    if (owner_.automation_profile_->automations.empty() &&
        owner_.automation_profile_->variables.empty())
      owner_.automation_profile_.reset();
  }
  owner_.automation_sidecar_state_ =
      owner_.automation_profile_ ? layerc::platform::AutomationSidecarState::valid
                          : layerc::platform::AutomationSidecarState::absent;
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
}
void AutomationEditor::initialize(const HWND dialog) {
  dialog_ = dialog;
  set_dark_title_bar(dialog);
  (void)layerc::platform::apply_capture_exclusion(dialog, owner_.capture_exclusion_);
  owner_.style_dialog_children(dialog);
  const HWND presets = GetDlgItem(dialog, IDC_AUTOMATION_PRESETS);
  for (const wchar_t *entry :
       {L"Input starts a cooldown", L"Repeating session timer",
        L"Ready overlay pulse"})
    SendMessageW(presets, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
  SendMessageW(presets, LB_SETCURSEL, 0, 0);

  const HWND property_event = GetDlgItem(dialog, IDC_AUTOMATION_EVENT);
  for (const auto kind : automation_event_kinds) {
    const auto label = event_label(kind);
    SendMessageW(property_event, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  const HWND property_action = GetDlgItem(dialog, IDC_AUTOMATION_ACTION_KIND);
  for (const auto kind : automation_action_kinds) {
    const auto label = action_label(kind);
    SendMessageW(property_action, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }

  const HWND palette = GetDlgItem(dialog, IDC_AUTOMATION_PALETTE);
  for (const wchar_t *entry :
       {L"Event: input pressed", L"Event: timer fired",
        L"Action: trigger cooldown", L"Action: create timer",
        L"Action: overlay visibility", L"Variable: Boolean"})
    SendMessageW(palette, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
  SendMessageW(palette, LB_SETCURSEL, 0, 0);
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
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_FIELDS),
           L"field:type=JSON; ...");
  refresh(dialog);
}

AutomationEditor::AutomationEditor(EditorWindow &owner) noexcept : owner_(owner) {}
AutomationEditor::~AutomationEditor() = default;

void AutomationEditor::open() {
  DialogBoxParamW(owner_.instance_, MAKEINTRESOURCEW(IDD_AUTOMATION),
                  owner_.dialog_, &dispatch_member_dialog<AutomationEditor,
                                          &AutomationEditor::initialize,
                                          &AutomationEditor::handle_message>,
                  reinterpret_cast<LPARAM>(this));
}

void AutomationEditor::add_basic(const HWND dialog) {
  if (owner_.profile_.cooldowns.empty()) {
    MessageBoxW(dialog,
                L"Add a cooldown before creating a Basic automation preset.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const int preset =
      std::max(0, static_cast<int>(SendDlgItemMessageW(
                      dialog, IDC_AUTOMATION_PRESETS, LB_GETCURSEL, 0, 0)));
  if (preset == 2 && owner_.profile_.panels.empty()) {
    MessageBoxW(dialog, L"The overlay preset requires at least one panel.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!owner_.automation_profile_) {
    layerc::core::AutomationProfile created;
    created.profile_id = owner_.profile_.id;
    owner_.automation_profile_ = std::move(created);
  }

  const auto id = unique_automation_id(*owner_.automation_profile_);
  layerc::core::AutomationDefinition definition;
  definition.id = id;
  definition.enabled = true;
  if (preset == 1) {
    definition.name = "Repeating session timer";
    layerc::core::AutomationRule start;
    start.id = "start-timer";
    start.event = layerc::core::AutomationEventKind::session_started;
    layerc::core::AutomationAction timer;
    timer.kind = layerc::core::AutomationActionKind::create_timer;
    timer.target = id + "-timer";
    timer.value =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    timer.repeating = true;
    start.actions.push_back(std::move(timer));
    layerc::core::AutomationRule tick;
    tick.id = "on-timer";
    tick.event = layerc::core::AutomationEventKind::timer_fired;
    tick.source_filter = id + "-timer";
    layerc::core::AutomationAction trigger;
    trigger.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    trigger.target = owner_.profile_.cooldowns.front().id;
    tick.actions.push_back(std::move(trigger));
    definition.rules = {std::move(start), std::move(tick)};
  } else if (preset == 2) {
    definition.name = "Ready overlay pulse";
    layerc::core::AutomationRule ready;
    ready.id = "on-ready";
    ready.event = layerc::core::AutomationEventKind::cooldown_ready;
    ready.source_filter = owner_.profile_.cooldowns.front().id;
    layerc::core::AutomationAction overlay;
    overlay.kind = layerc::core::AutomationActionKind::set_overlay_override;
    overlay.target = owner_.profile_.panels.front().id;
    overlay.name = id + "-ready";
    overlay.property = "visible";
    overlay.value = literal(layerc::core::AutomationValue(true));
    overlay.secondary =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    ready.actions.push_back(std::move(overlay));
    definition.rules.push_back(std::move(ready));
  } else {
    definition.name = "Input cooldown";
    layerc::core::AutomationRule input;
    input.id = "on-input";
    input.event = layerc::core::AutomationEventKind::input_pressed;
    input.source_filter = "keyboard:18";
    layerc::core::AutomationAction trigger;
    trigger.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    trigger.target = owner_.profile_.cooldowns.front().id;
    input.actions.push_back(std::move(trigger));
    definition.rules.push_back(std::move(input));
  }
  owner_.automation_profile_->automations.push_back(std::move(definition));
  owner_.automation_sidecar_state_ = layerc::platform::AutomationSidecarState::valid;
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_SETCURSEL,
                      owner_.automation_profile_->automations.size() - 1U, 0);
  graph_rule_ = 0;
  graph_action_ = -1;
  refresh(dialog);
}
void AutomationEditor::add_palette_node(const HWND dialog) {
  if (!owner_.automation_profile_ || selected_index(dialog) < 0) {
    add_basic(dialog);
    return;
  }
  const int selected = selected_index(dialog);
  auto before = *owner_.automation_profile_;
  auto &automation =
      owner_.automation_profile_->automations[static_cast<std::size_t>(selected)];
  const int palette = static_cast<int>(
      SendDlgItemMessageW(dialog, IDC_AUTOMATION_PALETTE, LB_GETCURSEL, 0, 0));
  if (palette >= 0 && palette <= 4 && owner_.profile_.cooldowns.empty()) {
    MessageBoxW(dialog, L"This node requires at least one cooldown.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (palette >= 2 && palette <= 4 && automation.rules.empty()) {
    MessageBoxW(dialog, L"Add an event node before adding an action.",
                L"Automation", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (palette == 0 || palette == 1) {
    layerc::core::AutomationRule rule;
    for (std::size_t suffix = automation.rules.size() + 1U;; ++suffix) {
      rule.id = "rule-" + std::to_string(suffix);
      if (std::ranges::none_of(automation.rules, [&](const auto &item) {
            return item.id == rule.id;
          }))
        break;
    }
    rule.event = palette == 0 ? layerc::core::AutomationEventKind::input_pressed
                              : layerc::core::AutomationEventKind::timer_fired;
    rule.source_filter = palette == 0 ? "keyboard:18" : "pulse";
    layerc::core::AutomationAction action;
    action.kind = palette == 0
                      ? layerc::core::AutomationActionKind::trigger_cooldown
                      : layerc::core::AutomationActionKind::reset_cooldown;
    action.target = owner_.profile_.cooldowns.front().id;
    rule.actions.push_back(std::move(action));
    automation.rules.push_back(std::move(rule));
  } else if (palette == 2) {
    layerc::core::AutomationAction action;
    action.kind = layerc::core::AutomationActionKind::trigger_cooldown;
    action.target = owner_.profile_.cooldowns.front().id;
    automation.rules.front().actions.push_back(std::move(action));
  } else if (palette == 3) {
    layerc::core::AutomationAction action;
    action.kind = layerc::core::AutomationActionKind::create_timer;
    action.target =
        "pulse-" + std::to_string(automation.rules.front().actions.size() + 1U);
    action.value =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    automation.rules.front().actions.push_back(std::move(action));
  } else if (palette == 4) {
    if (owner_.profile_.panels.empty()) {
      MessageBoxW(dialog, L"Overlay nodes require at least one panel.",
                  L"Automation", MB_OK | MB_ICONINFORMATION);
      return;
    }
    layerc::core::AutomationAction action;
    action.kind = layerc::core::AutomationActionKind::set_overlay_override;
    action.target = owner_.profile_.panels.front().id;
    action.name = "visibility-" +
                  std::to_string(automation.rules.front().actions.size() + 1U);
    action.property = "visible";
    action.value = literal(layerc::core::AutomationValue(true));
    action.secondary =
        literal(layerc::core::AutomationValue(layerc::core::DurationValue{1s}));
    automation.rules.front().actions.push_back(std::move(action));
  } else if (palette == 5) {
    std::string id;
    for (std::size_t suffix = owner_.automation_profile_->variables.size() + 1U;;
         ++suffix) {
      id = "flag-" + std::to_string(suffix);
      if (std::ranges::none_of(owner_.automation_profile_->variables,
                               [&](const auto &item) { return item.id == id; }))
        break;
    }
    owner_.automation_profile_->variables.push_back(
        {.id = id,
         .type = layerc::core::AutomationValueType::boolean,
         .default_value = layerc::core::AutomationValue(false)});
  } else {
    return;
  }
  const auto validation =
      layerc::core::validate_automation_profile(*owner_.automation_profile_, owner_.profile_);
  if (!validation.message.empty()) {
    *owner_.automation_profile_ = std::move(before);
    MessageBoxW(dialog,
                to_wide(validation.path + ": " + validation.message).c_str(),
                L"Node could not be added", MB_OK | MB_ICONWARNING);
    return;
  }
  if (palette == 0 || palette == 1) {
    graph_rule_ = static_cast<int>(automation.rules.size()) - 1;
    graph_action_ = -1;
  } else if (palette >= 2 && palette <= 4) {
    graph_rule_ = 0;
    graph_action_ =
        static_cast<int>(automation.rules.front().actions.size()) - 1;
  }
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_LIST, LB_SETCURSEL, selected, 0);
  refresh(dialog);
}

void AutomationEditor::remove_selected(const HWND dialog) {
  if (!owner_.automation_profile_)
    return;
  const int selected = selected_index(dialog);
  if (selected < 0 || static_cast<std::size_t>(selected) >=
                          owner_.automation_profile_->automations.size())
    return;
  owner_.automation_profile_->automations.erase(
      owner_.automation_profile_->automations.begin() + selected);
  graph_rule_ = -1;
  graph_action_ = -1;
  if (owner_.automation_profile_->automations.empty() &&
      owner_.automation_profile_->variables.empty())
    owner_.automation_profile_.reset();
  owner_.automation_sidecar_state_ =
      owner_.automation_profile_ ? layerc::platform::AutomationSidecarState::valid
                          : layerc::platform::AutomationSidecarState::absent;
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
}

bool AutomationEditor::apply_json(const HWND dialog) {
  const auto source =
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON)));
  const auto parsed =
      layerc::core::parse_automation_profile_json(source, owner_.profile_);
  const auto *profile = std::get_if<layerc::core::AutomationProfile>(&parsed);
  if (profile == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    MessageBoxW(dialog,
                (to_wide(error.path) + L": " + to_wide(error.message)).c_str(),
                L"Invalid automation graph", MB_OK | MB_ICONWARNING);
    return false;
  }
  owner_.automation_profile_ = *profile;
  graph_rule_ = -1;
  graph_action_ = -1;
  owner_.automation_sidecar_state_ = layerc::platform::AutomationSidecarState::valid;
  owner_.automation_sidecar_error_.clear();
  owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
  refresh(dialog);
  return true;
}

void AutomationEditor::validate_json(const HWND dialog) {
  const auto parsed = layerc::core::parse_automation_profile_json(
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON))), owner_.profile_);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_RESETCONTENT, 0, 0);
  std::wstring line = L"Valid - ready to apply";
  if (const auto *error = std::get_if<layerc::core::AutomationError>(&parsed))
    line = to_wide(error->path + ": " + error->message);
  SendDlgItemMessageW(dialog, IDC_AUTOMATION_VALIDATION, LB_ADDSTRING, 0,
                      reinterpret_cast<LPARAM>(line.c_str()));
}

void AutomationEditor::update_simulator_view(const HWND dialog) {
  std::string output =
      "Events: " + std::to_string(simulator_index_) + "/" +
      std::to_string(simulator_events_.size());
  if (!simulator_) {
    output += "\r\nReset the simulator to compile the current graph.";
  } else {
    output += "    Timers: " +
              std::to_string(simulator_->snapshot().timers.size()) +
              "\r\nVariables:";
    for (const auto &[id, value] : simulator_->snapshot().variables)
      output +=
          " " + id + "=" + layerc::core::automation_value_json(value) + ";";
    const auto assertions = to_utf8(
        control_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_ASSERTIONS)));
    if (!assertions.empty()) {
      bool all_passed = true;
      std::size_t cursor{};
      while (cursor < assertions.size()) {
        const auto end = assertions.find(';', cursor);
        const auto item =
            std::string_view(assertions)
                .substr(cursor, end == std::string::npos ? std::string::npos
                                                         : end - cursor);
        const auto equal = item.find('=');
        if (equal == std::string_view::npos || equal == 0) {
          all_passed = false;
          break;
        }
        const auto id = std::string(item.substr(0, equal));
        const auto &profile = simulator_->profile();
        const auto declaration = std::ranges::find(
            profile.variables, id, &layerc::core::AutomationVariable::id);
        if (declaration == profile.variables.end()) {
          all_passed = false;
          break;
        }
        const auto expected = layerc::core::parse_automation_value_json(
            item.substr(equal + 1U), declaration->type);
        const auto *value =
            std::get_if<layerc::core::AutomationValue>(&expected);
        const auto actual =
            simulator_->snapshot().variables.find(id);
        if (value == nullptr ||
            actual == simulator_->snapshot().variables.end() ||
            actual->second != *value)
          all_passed = false;
        if (end == std::string::npos)
          break;
        cursor = end + 1U;
        while (cursor < assertions.size() && assertions[cursor] == ' ')
          ++cursor;
      }
      output += all_passed ? "\r\nAssertions: PASS" : "\r\nAssertions: FAIL";
    }
  }
  trace_display_ = std::move(output);
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
           to_wide(trace_display_));
}

void AutomationEditor::reset_simulator(const HWND dialog) {
  KillTimer(dialog, automation_simulator_timer);
  simulator_playing_ = false;
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_PLAY), L"Play");
  const auto parsed = layerc::core::parse_automation_profile_json(
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_JSON))), owner_.profile_);
  const auto *graph = std::get_if<layerc::core::AutomationProfile>(&parsed);
  if (graph == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    trace_display_ =
        "Simulator reset failed: " + error.path + ": " + error.message;
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
    return;
  }
  simulator_ = std::make_unique<layerc::core::AutomationRuntime>(
      *graph, layerc::core::AutomationPersistentValues{},
      owner_.profile_.cooldown_groups);
  simulator_->set_trace_enabled(true);
  simulator_index_ = 0;
  if (simulator_events_.empty()) {
    layerc::core::AutomationEvent event;
    event.kind = layerc::core::AutomationEventKind::session_started;
    event.timestamp = layerc::core::TimePoint{};
    event.origin_token = 1;
    simulator_events_.push_back(std::move(event));
  }
  update_simulator_view(dialog);
}

void AutomationEditor::play_simulator(const HWND dialog,
                                      const bool playing) {
  KillTimer(dialog, automation_simulator_timer);
  simulator_playing_ = playing;
  if (playing) {
    if (!simulator_)
      reset_simulator(dialog);
    if (!simulator_ ||
        simulator_index_ >= simulator_events_.size()) {
      simulator_playing_ = false;
    } else {
      const int speed = static_cast<int>(SendDlgItemMessageW(
          dialog, IDC_AUTOMATION_SIM_SPEED, CB_GETCURSEL, 0, 0));
      SetTimer(dialog, automation_simulator_timer,
               speed == 0   ? 400U
               : speed == 2 ? 25U
                            : 100U,
               nullptr);
    }
  }
  set_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_PLAY),
           simulator_playing_ ? L"Pause" : L"Play");
}

void AutomationEditor::step_simulator(const HWND dialog) {
  if (!simulator_)
    reset_simulator(dialog);
  if (!simulator_ ||
      simulator_index_ >= simulator_events_.size()) {
    play_simulator(dialog, false);
    update_simulator_view(dialog);
    return;
  }
  const auto &event =
      simulator_events_[simulator_index_++];
  const auto trace_start = simulator_->trace().size();
  (void)simulator_->expire_overlay_overrides(event.timestamp);
  for (const auto &timer_event :
       simulator_->due_timer_events(event.timestamp))
    (void)simulator_->process(timer_event);
  const auto result = simulator_->process(event);
  if (!result.committed)
    simulator_playing_ = false;
  const auto breakpoint =
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_BREAKPOINT)));
  if (!breakpoint.empty()) {
    const auto &trace = simulator_->trace();
    for (std::size_t index = trace_start; index < trace.size(); ++index)
      if (trace[index].rule_id == breakpoint) {
        simulator_playing_ = false;
        break;
      }
  }
  if (!simulator_playing_ ||
      simulator_index_ >= simulator_events_.size())
    play_simulator(dialog, false);
  update_simulator_view(dialog);
}
void AutomationEditor::add_simulator_event(const HWND dialog) {
  constexpr auto &kinds = layerc::core::automation_event_kinds;
  const int selected = static_cast<int>(SendDlgItemMessageW(
      dialog, IDC_AUTOMATION_SIM_EVENT, CB_GETCURSEL, 0, 0));
  if (selected < 0 || static_cast<std::size_t>(selected) >= kinds.size())
    return;
  layerc::core::AutomationEvent event;
  event.kind = kinds[static_cast<std::size_t>(selected)];
  event.source_id =
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_SOURCE)));
  const auto timestamp =
      simulator_events_.empty()
          ? 0ms
          : std::chrono::duration_cast<layerc::core::Milliseconds>(
                simulator_events_.back()
                    .timestamp.time_since_epoch()) +
                100ms;
  event.timestamp = layerc::core::TimePoint(timestamp);
  event.origin_token = simulator_events_.size() + 1U;
  auto fields =
      to_utf8(control_text(GetDlgItem(dialog, IDC_AUTOMATION_SIM_FIELDS)));
  if (fields != "field:type=JSON; ..." && !fields.empty()) {
    std::size_t cursor{};
    while (cursor < fields.size()) {
      const auto end = fields.find(';', cursor);
      const auto item = std::string_view(fields).substr(
          cursor, end == std::string::npos ? std::string::npos : end - cursor);
      const auto colon = item.find(':');
      const auto equal = item.find('=');
      if (colon == std::string_view::npos || equal == std::string_view::npos ||
          colon == 0 || equal <= colon + 1U) {
        MessageBoxW(dialog,
                    L"Fields use name:type=JSON; for example value:number=0.5",
                    L"Synthetic event", MB_OK | MB_ICONWARNING);
        return;
      }
      const auto type = layerc::core::parse_automation_value_type_name(
          item.substr(colon + 1U, equal - colon - 1U));
      if (!type.has_value()) {
        MessageBoxW(dialog, L"Unsupported synthetic field type.",
                    L"Synthetic event", MB_OK | MB_ICONWARNING);
        return;
      }
      const auto parsed = layerc::core::parse_automation_value_json(
          item.substr(equal + 1U), *type);
      const auto *value = std::get_if<layerc::core::AutomationValue>(&parsed);
      if (value == nullptr) {
        const auto &error = std::get<layerc::core::AutomationError>(parsed);
        MessageBoxW(dialog, to_wide(error.path + ": " + error.message).c_str(),
                    L"Synthetic event", MB_OK | MB_ICONWARNING);
        return;
      }
      event.fields.emplace(std::string(item.substr(0, colon)), *value);
      if (end == std::string::npos)
        break;
      cursor = end + 1U;
      while (cursor < fields.size() && fields[cursor] == ' ')
        ++cursor;
    }
  }
  simulator_events_.push_back(event);
  trace_events_.push_back(std::move(event));
  update_simulator_view(dialog);
}

void AutomationEditor::load_trace(const HWND dialog) {
  const auto path = choose_trace_path(dialog, false);
  if (!path.has_value())
    return;
  const auto contents = read_bounded_trace(*path);
  if (!contents.has_value()) {
    MessageBoxW(dialog, L"The trace could not be read or exceeds 4 MiB.",
                L"Load trace", MB_OK | MB_ICONERROR);
    return;
  }
  const auto parsed = layerc::core::parse_layerc_trace(*contents);
  const auto *events =
      std::get_if<std::vector<layerc::core::AutomationEvent>>(&parsed);
  if (events == nullptr) {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    MessageBoxW(dialog, to_wide(error.path + ": " + error.message).c_str(),
                L"Invalid trace", MB_OK | MB_ICONWARNING);
    return;
  }
  trace_json_ = *contents;
  trace_events_ = *events;
  simulator_events_ = *events;
  reset_simulator(dialog);
}

void AutomationEditor::draw_canvas(const DRAWITEMSTRUCT &item) const {
  const auto canvas_colors =
      owner_.colors(layerc::platform::EditorControlRole::preview);
  const auto list_colors = owner_.colors(layerc::platform::EditorControlRole::list);
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
      owner_.automation_profile_->automations[static_cast<std::size_t>(selected)];
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
    const bool selected_node = node.rule == graph_rule_ &&
                               node.action == graph_action_;
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
bool AutomationEditor::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type !=
      layerc::core::LiveMessageType::automation_trace_event)
    return false;

  const auto parsed = layerc::core::parse_layerc_trace(message.text);
  if (const auto *events =
          std::get_if<std::vector<layerc::core::AutomationEvent>>(&parsed)) {
    constexpr std::size_t max_trace_events = 2048U;
    const std::size_t incoming = std::min(events->size(), max_trace_events);
    const std::size_t retained = max_trace_events - incoming;
    if (trace_events_.size() > retained)
      trace_events_.erase(
          trace_events_.begin(),
          trace_events_.end() - static_cast<std::ptrdiff_t>(retained));
    trace_events_.insert(
        trace_events_.end(),
        events->end() - static_cast<std::ptrdiff_t>(incoming), events->end());
    trace_json_ = message.text;
    trace_display_ =
        "Live trace events: " + std::to_string(trace_events_.size()) +
        "\r\nLast authenticated packet: " +
        std::to_string(message.text.size()) + " bytes";
  } else {
    const auto &error = std::get<layerc::core::AutomationError>(parsed);
    trace_display_ = "Rejected malformed LayerD trace: " + error.path +
                     ": " + error.message;
  }
  if (dialog_ != nullptr && IsWindow(dialog_))
    set_text(GetDlgItem(dialog_, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
  return true;
}

void AutomationEditor::restore_live_subscription() {
  if (!trace_enabled_)
    return;
  ++owner_.revision_;
  (void)owner_.live_channel_.send({
      .type = layerc::core::LiveMessageType::automation_trace_subscribe,
      .revision = owner_.revision_,
  });
}
void AutomationEditor::set_trace(const HWND dialog, const bool enabled) {
  trace_enabled_ = enabled;
  set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), enabled);
  if (enabled) {
    trace_json_.clear();
    trace_events_.clear();
    trace_display_ = "Waiting for authenticated LayerD events...";
  }
  if (!owner_.live_channel_.connected() || !owner_.live_package_authenticated_) {
    trace_display_ =
        "LayerD is not authenticated; start or restart LayerD and try again.";
    set_text(GetDlgItem(dialog, IDC_AUTOMATION_TRACE_TEXT),
             to_wide(trace_display_));
    trace_enabled_ = false;
    set_owner_draw_checkbox(GetDlgItem(dialog, IDC_AUTOMATION_TRACE), false);
    return;
  }
  ++owner_.revision_;
  layerc::core::LiveMessage message;
  message.type =
      enabled ? layerc::core::LiveMessageType::automation_trace_subscribe
              : layerc::core::LiveMessageType::automation_trace_unsubscribe;
  message.revision = owner_.revision_;
  (void)owner_.live_channel_.send(message);
}

void AutomationEditor::save_trace(const HWND dialog) {
  if (trace_events_.empty() && trace_json_.empty()) {
    MessageBoxW(dialog, L"There is no trace or simulator output to save.",
                L"Automation trace", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (MessageBoxW(
          dialog,
          L"Trace files retain typed source-field values for exact replay and "
          L"may include user-authored strings or maps. Save this trace?",
          L"Automation trace privacy", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
    return;
  const auto path = choose_trace_path(dialog, true);
  if (!path.has_value())
    return;
  const auto trace = trace_events_.empty()
                         ? trace_json_
                         : trace_from_events(trace_events_);
  std::ofstream output(*path, std::ios::binary | std::ios::trunc);
  output.write(trace.data(), static_cast<std::streamsize>(trace.size()));
  if (!output)
    MessageBoxW(dialog, L"Could not save the trace file.", L"Automation trace",
                MB_OK | MB_ICONERROR);
}

void AutomationEditor::retry_selected(const HWND dialog) {
  if (!owner_.automation_profile_)
    return;
  const int selected = selected_index(dialog);
  if (selected < 0 || static_cast<std::size_t>(selected) >=
                          owner_.automation_profile_->automations.size())
    return;
  ++owner_.revision_;
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::retry_automation;
  message.revision = owner_.revision_;
  message.profile_key =
      to_utf8(layerc::platform::canonical_profile_key(owner_.current_path_));
  message.text =
      owner_.automation_profile_->automations[static_cast<std::size_t>(selected)].id;
  (void)owner_.live_channel_.send(message);
}

LRESULT AutomationEditor::handle_message(const HWND window,
                                          const UINT message,
                                          const WPARAM wparam,
                                          const LPARAM lparam) {
  if (message == WM_COMMAND) {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    switch (id) {
    case IDC_AUTOMATION_LIST:
      if (notification == LBN_SELCHANGE) {
        graph_rule_ = -1;
        graph_action_ = -1;
        refresh(window);
      }
      return TRUE;
    case IDC_AUTOMATION_PRESETS:
      if (notification == LBN_DBLCLK)
        add_basic(window);
      return TRUE;
    case IDC_AUTOMATION_CANVAS:
      if (notification == STN_CLICKED)
        select_canvas_node(window);
      return TRUE;
    case IDC_AUTOMATION_APPLY_NODE:
      apply_node_properties(window);
      return TRUE;
    case IDC_AUTOMATION_DELETE_NODE:
      delete_canvas_node(window);
      return TRUE;
    case IDC_AUTOMATION_ADD_PRESET:
      add_basic(window);
      return TRUE;
    case IDC_AUTOMATION_REMOVE:
      remove_selected(window);
      return TRUE;
    case IDC_AUTOMATION_ENABLED:
      if (notification == BN_CLICKED && owner_.automation_profile_) {
        const int selected = selected_index(window);
        if (selected >= 0) {
          owner_.automation_profile_
              ->automations[static_cast<std::size_t>(selected)]
              .enabled = owner_draw_checkbox_checked(
              GetDlgItem(window, IDC_AUTOMATION_ENABLED));
          owner_.record_profile_change(EditorWindow::ChangeKind::immediate);
          refresh(window);
        }
      }
      return TRUE;
    case IDC_AUTOMATION_NAME:
      if (notification == EN_KILLFOCUS && owner_.automation_profile_) {
        const int selected = selected_index(window);
        if (selected >= 0) {
          owner_.automation_profile_
              ->automations[static_cast<std::size_t>(selected)]
              .name =
              to_utf8(control_text(GetDlgItem(window, IDC_AUTOMATION_NAME)));
          owner_.record_profile_change(EditorWindow::ChangeKind::text);
        }
        refresh(window);
      }
      return TRUE;
    case IDC_AUTOMATION_APPLY:
      (void)apply_json(window);
      return TRUE;
    case IDC_AUTOMATION_ADD_NODE:
      add_palette_node(window);
      return TRUE;
    case IDC_AUTOMATION_VALIDATE:
      validate_json(window);
      return TRUE;
    case IDC_AUTOMATION_ZOOM_OUT:
      graph_zoom_ =
          std::max(50, graph_zoom_ - 25);
      refresh_graph(window);
      return TRUE;
    case IDC_AUTOMATION_ZOOM_IN:
      graph_zoom_ =
          std::min(200, graph_zoom_ + 25);
      refresh_graph(window);
      return TRUE;
    case IDC_AUTOMATION_TRACE_LOAD:
      load_trace(window);
      return TRUE;
    case IDC_AUTOMATION_SIM_ADD:
      add_simulator_event(window);
      return TRUE;
    case IDC_AUTOMATION_SIM_RESET:
      reset_simulator(window);
      return TRUE;
    case IDC_AUTOMATION_SIM_STEP:
      step_simulator(window);
      return TRUE;
    case IDC_AUTOMATION_SIM_PLAY:
      play_simulator(window, !simulator_playing_);
      return TRUE;
    case IDC_AUTOMATION_TRACE:
      if (notification == BN_CLICKED)
        set_trace(
            window, owner_draw_checkbox_checked(
                        GetDlgItem(window, IDC_AUTOMATION_TRACE)));
      return TRUE;
    case IDC_AUTOMATION_SAVE_TRACE:
      save_trace(window);
      return TRUE;
    case IDC_AUTOMATION_RETRY:
      retry_selected(window);
      return TRUE;
    case IDOK:
    case IDCANCEL:
      if (trace_enabled_)
        set_trace(window, false);
      dialog_ = nullptr;
      EndDialog(window, id);
      return TRUE;
    default:
      break;
    }
  }
  if (message == WM_TIMER && wparam == automation_simulator_timer) {
    step_simulator(window);
    return TRUE;
  }
  if (message == WM_DRAWITEM) {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (item.CtlID == IDC_AUTOMATION_LIST)
      owner_.draw_list_box_item(item);
    else if (item.CtlID == IDC_AUTOMATION_CANVAS)
      draw_canvas(item);
    else if (item.CtlType == ODT_LISTBOX)
      owner_.draw_list_box_item(item);
    else
      owner_.draw_button(item, layerc::platform::EditorControlRole::button);
    return TRUE;
  }
  if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
    return owner_.color_message(message, wparam, lparam);
  if (message == WM_DESTROY) {
    KillTimer(window, automation_simulator_timer);
    simulator_playing_ = false;
    dialog_ = nullptr;
  }
  return FALSE;
}

} // namespace layerc::editor