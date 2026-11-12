#include "editor_window.hpp"
#include "editor_resource.h"
#include "dialog_layout.hpp"
#include "static_style.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {
std::wstring number_text(const double value) {
  std::array<char, 64> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return error == std::errc{} ? to_wide(std::string_view(buffer.data(), end)) : L"";
}
std::string source_id(const core::OverlayItem &item) {
  if (const auto *value = std::get_if<core::CooldownItem>(&item.content)) return value->cooldown_id;
  if (const auto *value = std::get_if<core::CooldownActiveTimerItem>(&item.content)) return value->cooldown_id;
  if (const auto *value = std::get_if<core::AutomationTimerItem>(&item.content)) return value->timer_id;
  if (const auto *value = std::get_if<core::CounterItem>(&item.content)) return value->variable;
  if (const auto *value = std::get_if<core::IndicatorItem>(&item.content)) return value->variable;
  return {};
}
void select_combo(const HWND window, const int id, const int selection) {
  SendDlgItemMessageW(window, id, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
}
int combo_selection(const HWND window, const int id) {
  return static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0));
}
void append_combo(const HWND window, const int id, const std::wstring &text) {
  SendDlgItemMessageW(window, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}
}

struct PanelItemDialogState {
  EditorWindow &owner;
  std::size_t panel{};
  std::optional<std::size_t> index;
  core::OverlayItem item;
  std::string profile_id;
  std::string panel_id;
  HWND window{};
  DialogChrome chrome;
  std::vector<std::string> sources;
  std::vector<double> flash_values{0, 0.5, 1, 2, 3};

  int kind() const { return combo_selection(window, IDC_ITEM_KIND); }
  bool automation_timer() const {
    return kind() == 1 && combo_selection(window, IDC_ITEM_TIMER_SOURCE) == 1;
  }
  bool preset() const {
    return !index && automation_timer() &&
        IsDlgButtonChecked(window, IDC_ITEM_PRESET) == BST_CHECKED;
  }
  std::string selected_source() const {
    const auto selection = combo_selection(window, IDC_ITEM_SOURCE);
    return selection < 0 || static_cast<std::size_t>(selection) >= sources.size()
        ? std::string{} : sources[static_cast<std::size_t>(selection)];
  }
  std::optional<double> selected_flash() const {
    const int selection = combo_selection(window, IDC_ITEM_FLASH);
    if (selection < 0 || static_cast<std::size_t>(selection) >= flash_values.size())
      return std::nullopt;
    return flash_values[static_cast<std::size_t>(selection)];
  }
  void set_feedback(const std::optional<layerc::platform::StatusGlyph> glyph,
                    const std::wstring_view text) const {
    const HWND label = GetDlgItem(window, IDC_ITEM_ERROR);
    set_status_line(label, glyph, text);
    for (const int id : {IDC_ITEM_KIND, IDC_ITEM_TIMER_SOURCE, IDC_ITEM_PRESET,
                         IDC_ITEM_SOURCE, IDC_ITEM_CUSTOM_LABEL, IDC_ITEM_LABEL,
                         IDC_ITEM_MAX, IDC_ITEM_DISPLAY, IDC_ITEM_FLASH,
                         IDC_ITEM_INTERVAL})
      describe_with(GetDlgItem(window, id), label);
  }
  void refresh_flash_warning() const {
    if (kind() == 3 &&
        selected_flash().value_or(0.0) > core::max_shown_indicator_flash_hz) {
      set_feedback(layerc::platform::StatusGlyph::warning,
                   L"Flashes at 3 Hz; faster flashing is capped.");
      return;
    }
    set_feedback(std::nullopt, {});
  }
  void error(const std::string_view text) {
    if (text.empty()) {
      refresh_flash_warning();
      return;
    }
    set_feedback(layerc::platform::StatusGlyph::no, to_wide(text));
  }
  void refresh_sources(const bool first = false) {
    const auto previous = first ? source_id(item) : selected_source();
    sources.clear();
    SendDlgItemMessageW(window, IDC_ITEM_SOURCE, CB_RESETCONTENT, 0, 0);
    const auto append = [&](const std::string &id, const std::string &label) {
      if (std::ranges::find(sources, id) != sources.end()) return;
      sources.push_back(id);
      append_combo(window, IDC_ITEM_SOURCE, to_wide(label));
    };
    const auto &profile = owner.document_.profile();
    const auto &graph = owner.document_.automation();
    if (kind() == 0 || (kind() == 1 && (!automation_timer() || preset()))) {
      for (const auto &cooldown : profile.cooldowns) append(cooldown.id, cooldown.name);
    } else if (graph && kind() == 1) {
      for (const auto &automation : graph->automations)
        for (const auto &rule : automation.rules)
          for (const auto &action : rule.actions)
            if (action.kind == core::AutomationActionKind::create_timer)
              append(action.target, action.target);
    } else if (graph) {
      for (const auto &variable : graph->variables) {
        const bool numeric = variable.type == core::AutomationValueType::integer ||
                             variable.type == core::AutomationValueType::number;
        if (numeric || (kind() == 3 && variable.type == core::AutomationValueType::boolean))
          append(variable.id, variable.id);
      }
    }
    // Preserve an unavailable existing binding while editing its label. Do not
    // offer that binding when the user changes to another source/kind.
    const auto original_kind = core::overlay_item_kind(item);
    const bool same_timer_source = kind() != 1 || automation_timer() ==
        std::holds_alternative<core::AutomationTimerItem>(item.content);
    const auto original_source = source_id(item);
    if (index && original_kind && kind() == static_cast<int>(*original_kind) &&
        same_timer_source && !original_source.empty() &&
        std::ranges::find(sources, original_source) == sources.end())
      append(original_source, original_source + " (unavailable)");
    const auto found = std::ranges::find(sources, previous);
    select_combo(window, IDC_ITEM_SOURCE,
                 found == sources.end() ? (sources.empty() ? -1 : 0)
                                        : static_cast<int>(found - sources.begin()));
  }
  void refresh_controls(const bool first = false) {
    const bool timer = kind() == 1;
    const bool counter = kind() == 2;
    const bool indicator = kind() == 3;
    const auto show = [&](const int id, const bool visible) {
      ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE);
      EnableWindow(GetDlgItem(window, id), visible);
    };
    show(IDC_ITEM_TIMER_LABEL, timer);
    show(IDC_ITEM_TIMER_SOURCE, timer);
    show(IDC_ITEM_PRESET, automation_timer() && !index);
    for (const auto id : {IDC_ITEM_MAX, IDC_ITEM_MAX_LABEL,
                          IDC_ITEM_DISPLAY, IDC_ITEM_DISPLAY_LABEL}) show(id, counter);
    show(IDC_ITEM_FLASH, indicator);
    show(IDC_ITEM_FLASH_LABEL, indicator);
    show(IDC_ITEM_INTERVAL, preset());
    show(IDC_ITEM_INTERVAL_LABEL, preset());
    EnableWindow(GetDlgItem(window, IDC_ITEM_LABEL),
                 IsDlgButtonChecked(window, IDC_ITEM_CUSTOM_LABEL) == BST_CHECKED);
    error({});
    refresh_sources(first);
    layout();
  }
  void layout() const {
    const DialogLayout ui(window, window_base_dpi(window));
    RECT client{}; GetClientRect(window, &client);
    const int margin = ui.scaled(12), label = ui.scaled(112);
    const int height = ui.scaled(native_control_height), pitch = ui.scaled(34);
    const int width = client.right - margin * 2;
    int top = margin;
    const auto field = [&](const int label_id, const int control_id, const int y) {
      ui.field_row(label_id, control_id, margin, y, label, width, height);
      // The drop-down's height includes its popup list.
      if (control_id == IDC_ITEM_KIND || control_id == IDC_ITEM_TIMER_SOURCE ||
          control_id == IDC_ITEM_SOURCE || control_id == IDC_ITEM_FLASH)
        ui.move(control_id, margin + label, y, width - label, ui.scaled(180));
    };
    field(IDC_ITEM_KIND_LABEL, IDC_ITEM_KIND, top); top += pitch;
    if (kind() == 1) {
      field(IDC_ITEM_TIMER_LABEL, IDC_ITEM_TIMER_SOURCE, top); top += pitch;
      if (automation_timer() && !index) {
        ui.move(IDC_ITEM_PRESET, margin + label, top, width - label, height);
        top += pitch;
      }
    }
    field(IDC_ITEM_SOURCE_LABEL, IDC_ITEM_SOURCE, top); top += pitch;
    ui.move(IDC_ITEM_CUSTOM_LABEL, margin, top, label - ui.scaled(5), height);
    ui.move(IDC_ITEM_LABEL, margin + label, top, width - label, height); top += pitch;
    if (kind() == 2) {
      ui.row_label(IDC_ITEM_MAX_LABEL, margin, top, label - ui.scaled(5), height);
      ui.move(IDC_ITEM_MAX, margin + label, top, ui.scaled(84), height);
      ui.row_label(IDC_ITEM_DISPLAY_LABEL, margin + label + ui.scaled(100), top,
                   ui.scaled(60), height);
      ui.move(IDC_ITEM_DISPLAY, margin + label + ui.scaled(160), top,
               std::max(1, width - label - ui.scaled(160)), ui.scaled(100));
      top += pitch;
    }
    if (kind() == 3) {
      field(IDC_ITEM_FLASH_LABEL, IDC_ITEM_FLASH, top); top += pitch;
    }
    if (preset()) {
      field(IDC_ITEM_INTERVAL_LABEL, IDC_ITEM_INTERVAL, top); top += pitch;
    }
    ui.move(IDC_ITEM_ERROR, margin, top, width, ui.scaled(64));
    const int buttons = client.bottom - margin - height;
    ui.move(IDOK, client.right - margin - ui.scaled(220), buttons, ui.scaled(120), height);
    ui.move(IDCANCEL, client.right - margin - ui.scaled(92), buttons, ui.scaled(92), height);
  }
  void initialize() {
    (void)chrome.attach(window);
    apply_dialog_chrome(window, owner.shell_.capture_exclusion);
    if (const HWND label = GetDlgItem(window, IDC_ITEM_ERROR); label != nullptr)
      SetWindowLongPtrW(
          label, GWL_STYLE,
          (GetWindowLongPtrW(label, GWL_STYLE) & ~static_cast<LONG_PTR>(SS_TYPEMASK)) |
              SS_OWNERDRAW | SS_NOPREFIX);
    resize_to_logical_size(window, 540, 430);
    SetWindowTextW(window, index ? L"Edit overlay item" : L"Add overlay item");
    for (const auto text : {L"Cooldown", L"Timer", L"Counter", L"Indicator"})
      append_combo(window, IDC_ITEM_KIND, text);
    append_combo(window, IDC_ITEM_TIMER_SOURCE, L"Cooldown active duration");
    append_combo(window, IDC_ITEM_TIMER_SOURCE, L"Automation timer");
    append_combo(window, IDC_ITEM_DISPLAY, L"Number");
    append_combo(window, IDC_ITEM_DISPLAY, L"Pips");
    select_combo(window, IDC_ITEM_KIND,
                 static_cast<int>(core::overlay_item_kind(item).value_or(core::OverlayItemKind::cooldown)));
    select_combo(window, IDC_ITEM_TIMER_SOURCE,
                 std::holds_alternative<core::AutomationTimerItem>(item.content) ? 1 : 0);
    CheckDlgButton(window, IDC_ITEM_PRESET, index ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(window, IDC_ITEM_CUSTOM_LABEL, item.label ? BST_CHECKED : BST_UNCHECKED);
    set_text(GetDlgItem(window, IDC_ITEM_LABEL), to_wide(item.label.value_or("")));
    SendDlgItemMessageW(window, IDC_ITEM_LABEL, EM_SETLIMITTEXT, 128, 0);
    select_combo(window, IDC_ITEM_DISPLAY, 0);
    if (const auto *counter = std::get_if<core::CounterItem>(&item.content)) {
      select_combo(window, IDC_ITEM_DISPLAY, counter->display == core::CounterDisplay::pips ? 1 : 0);
      set_text(GetDlgItem(window, IDC_ITEM_MAX), counter->max ? number_text(*counter->max) : L"");
    }
    const auto *indicator = std::get_if<core::IndicatorItem>(&item.content);
    const double flash = indicator == nullptr ? 0 : indicator->flash_hz;
    if (std::ranges::find(flash_values, flash) == flash_values.end()) flash_values.push_back(flash);
    for (const double value : flash_values)
      append_combo(window, IDC_ITEM_FLASH, value == 0 ? L"Off" : number_text(value));
    select_combo(window, IDC_ITEM_FLASH,
                 static_cast<int>(std::ranges::find(flash_values, flash) - flash_values.begin()));
    set_text(GetDlgItem(window, IDC_ITEM_INTERVAL), L"10");
    refresh_controls(true);
    layout();
  }
  bool apply() {
    const auto &profile = owner.document_.profile();
    if (profile.id != profile_id || panel >= profile.panels.size() ||
        profile.panels[panel].id != panel_id) {
      error("The selected profile or panel changed. Reopen the item dialog.");
      return false;
    }
    const auto source = selected_source();
    if (source.empty()) { error("Select a compatible source first."); return false; }
    auto candidate = item;
    candidate.label = IsDlgButtonChecked(window, IDC_ITEM_CUSTOM_LABEL) == BST_CHECKED
        ? std::optional<std::string>(layerc::platform::wide_to_utf8(
              window_text(GetDlgItem(window, IDC_ITEM_LABEL))))
        : std::nullopt;
    std::optional<core::Milliseconds> interval;
    switch (kind()) {
    case 0: candidate.content = core::CooldownItem{source}; break;
    case 1:
      if (automation_timer()) candidate.content = core::AutomationTimerItem{source};
      else candidate.content = core::CooldownActiveTimerItem{source};
      if (preset()) {
        interval = parse_duration_seconds(layerc::platform::wide_to_utf8(
            window_text(GetDlgItem(window, IDC_ITEM_INTERVAL))));
        if (!interval || *interval < core::min_automation_timer_interval) {
          error("Timer interval must be from 0.05 seconds to the maximum cooldown duration.");
          return false;
        }
      }
      break;
    case 2: {
      const auto text = layerc::platform::wide_to_utf8(window_text(GetDlgItem(window, IDC_ITEM_MAX)));
      std::optional<double> maximum;
      if (!text.empty()) {
        double value{};
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (code != std::errc{} || end != text.data() + text.size() ||
            !std::isfinite(value) || value <= 0) {
          error("Counter max must be empty, or a finite number greater than zero.");
          return false;
        }
        maximum = value;
      }
      candidate.content = core::CounterItem{source, maximum,
          combo_selection(window, IDC_ITEM_DISPLAY) == 1
              ? core::CounterDisplay::pips : core::CounterDisplay::number};
      break;
    }
    case 3: {
      const int selection = combo_selection(window, IDC_ITEM_FLASH);
      if (selection < 0 || static_cast<std::size_t>(selection) >= flash_values.size()) {
        error("Select Off or a flash frequency."); return false;
      }
      candidate.content = core::IndicatorItem{source, flash_values[static_cast<std::size_t>(selection)]};
      break;
    }
    default: error("Select an item kind."); return false;
    }
    std::string message;
    if (!owner.commit_panel_item(panel, index, std::move(candidate), interval, message)) {
      error(message); return false;
    }
    return true;
  }
};

void EditorWindow::open_panel_item_dialog(const bool editing) {
  if (!draft_controls_valid() || !apply_profile_controls(ChangeKind::text)) {
    set_status(L"Correct invalid profile fields before editing items.");
    return;
  }
  const auto *panel = selected_bar();
  if (panel == nullptr) { set_status(L"Select a panel first."); return; }
  std::optional<std::size_t> index;
  core::OverlayItem item;
  if (editing) {
    const auto row = SendDlgItemMessageW(shell_.window, IDC_PANEL_ITEMS, LB_GETCURSEL, 0, 0);
    if (row < 0) return;
    index = static_cast<std::size_t>(row);
    if (panel->items) {
      if (*index >= panel->items->size()) return;
      item = (*panel->items)[*index];
      if (!core::overlay_item_kind(item)) return;
    } else {
      if (*index >= panel->cooldown_ids.size()) return;
      item.content = core::CooldownItem{panel->cooldown_ids[*index]};
    }
  }
  PanelItemDialogState state{.owner = *this,
      .panel = static_cast<std::size_t>(panel - document_.profile().panels.data()),
      .index = index, .item = std::move(item),
      .profile_id = document_.profile().id, .panel_id = panel->id};
  DialogBoxParamW(shell_.instance, MAKEINTRESOURCEW(IDD_PANEL_ITEM), shell_.window,
                   &EditorWindow::panel_item_dialog_proc,
                   reinterpret_cast<LPARAM>(&state));
}

INT_PTR CALLBACK EditorWindow::panel_item_dialog_proc(const HWND window,
    const UINT message, const WPARAM wparam, const LPARAM lparam) {
  auto *state = reinterpret_cast<PanelItemDialogState *>(GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<PanelItemDialogState *>(lparam);
    if (state == nullptr) return FALSE;
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    state->window = window;
    state->initialize();
    return TRUE;
  }
  if (state == nullptr) return FALSE;
  switch (message) {
  case WM_COMMAND: {
    const int id = LOWORD(wparam), notification = HIWORD(wparam);
    if (id == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
    if (id == IDOK) {
      if (state->apply()) EndDialog(window, IDOK);
      return TRUE;
    }
    if (((id == IDC_ITEM_KIND || id == IDC_ITEM_TIMER_SOURCE) && notification == CBN_SELCHANGE) ||
        ((id == IDC_ITEM_PRESET || id == IDC_ITEM_CUSTOM_LABEL) && notification == BN_CLICKED)) {
      state->refresh_controls();
      return TRUE;
    }
    if (id == IDC_ITEM_FLASH && notification == CBN_SELCHANGE) {
      state->refresh_flash_warning();
      return TRUE;
    }
    break;
  }
  case WM_SIZE: state->layout(); return TRUE;
  case WM_DPICHANGED:
    (void)state->chrome.on_dpi_changed(HIWORD(wparam), lparam);
    state->layout(); return TRUE;
  case WM_CLOSE: EndDialog(window, IDCANCEL); return TRUE;
  case WM_DESTROY: state->chrome.on_destroy(); return TRUE;
  default: break;
  }
  return state->owner.dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
