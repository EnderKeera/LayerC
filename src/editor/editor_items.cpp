#include "editor_window.hpp"
#include "editor_resource.h"
#include "panel_items.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <variant>

namespace layerc::editor {
namespace {
std::size_t row_count(const core::Panel &panel) {
  return panel.items ? panel.items->size() : panel.cooldown_ids.size();
}
}

void EditorWindow::refresh_panel_items(const std::size_t preferred) {
  auto sync = shell_.sync_scope();
  const HWND list = GetDlgItem(shell_.window, IDC_PANEL_ITEMS);
  const auto previous = SendMessageW(list, LB_GETCURSEL, 0, 0);
  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  const auto *panel = selected_bar();
  if (panel != nullptr) {
    const auto append = [&](const core::OverlayItem &item) {
      auto label = to_wide(item_kind_text(item) + ": " +
                           item_display_name(document_.profile(), item));
      if (std::holds_alternative<core::OpaqueOverlayItem>(item.content))
        label += L" (unsupported kind)";
      SendMessageW(list, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(label.c_str()));
    };
    if (panel->items) {
      for (const auto &item : *panel->items) append(item);
    } else {
      for (const auto &id : panel_cooldown_members(*panel))
        append(core::OverlayItem{.id = {},
                                 .content = core::CooldownItem{id}});
    }
    const auto count = row_count(*panel);
    if (count != 0) {
      const auto selection = preferred != SIZE_MAX
                                 ? preferred
                                 : previous < 0 ? 0U
                                                : static_cast<std::size_t>(previous);
      SendMessageW(list, LB_SETCURSEL,
                   std::min(selection, count - 1), 0);
    }
  }
  EnableWindow(list, panel != nullptr);
  refresh_item_actions();
}

void EditorWindow::refresh_item_actions() {
  const auto *panel = selected_bar();
  const auto selected = SendDlgItemMessageW(
      shell_.window, IDC_PANEL_ITEMS, LB_GETCURSEL, 0, 0);
  const auto count = panel == nullptr ? 0 : row_count(*panel);
  const bool has_row = selected >= 0 &&
                       static_cast<std::size_t>(selected) < count;
  const bool opaque = has_row && panel->items &&
      std::holds_alternative<core::OpaqueOverlayItem>(
          (*panel->items)[static_cast<std::size_t>(selected)].content);
  EnableWindow(GetDlgItem(shell_.window, IDC_ITEM_ADD),
               panel != nullptr && count < core::max_overlay_items_per_panel);
  EnableWindow(GetDlgItem(shell_.window, IDC_ITEM_EDIT), has_row && !opaque);
  EnableWindow(GetDlgItem(shell_.window, IDC_ITEM_REMOVE), has_row && count > 1);
  EnableWindow(GetDlgItem(shell_.window, IDC_ITEM_UP), has_row && selected > 0);
  EnableWindow(GetDlgItem(shell_.window, IDC_ITEM_DOWN),
               has_row && static_cast<std::size_t>(selected) + 1 < count);
}

bool EditorWindow::commit_panel_item(
    const std::size_t panel, const std::optional<std::size_t> index,
    core::OverlayItem item, const std::optional<core::Milliseconds> interval,
    std::string &error) {
  const auto outcome = document_.edit_profile_and_automation(
      [&](core::GameProfile &profile,
          std::optional<core::AutomationProfile> &automation)
          -> std::optional<std::string> {
        if (interval) {
          const auto *source = std::get_if<core::AutomationTimerItem>(&item.content);
          if (source == nullptr || index) return "Invalid timer preset.";
          const auto result = add_automation_timer_preset(
              profile, automation, panel, source->timer_id, *interval, item.label);
          if (const auto *failure = std::get_if<ItemEditError>(&result))
            return failure->message;
        } else {
          const auto failure = index
              ? update_panel_item(profile, automation, panel, *index, item)
              : add_panel_item(profile, automation, panel, item);
          if (failure) return failure->message;
        }
        return std::nullopt;
      }, ChangeKind::immediate);
  if (!apply_edit(outcome, ChangeKind::immediate)) {
    error = outcome.message;
    return false;
  }
  refresh_bar_list(panel);
  const auto &updated = document_.profile().panels[panel];
  refresh_panel_items(index.value_or(row_count(updated) - 1));
  layout();
  return true;
}

bool EditorWindow::handle_item_command(const int id, const int notification) {
  if (id == IDC_PANEL_ITEMS) {
    if (notification == LBN_SELCHANGE) refresh_item_actions();
    if (notification == LBN_DBLCLK) open_panel_item_dialog(true);
    return true;
  }
  if (id < IDC_ITEM_ADD || id > IDC_ITEM_DOWN) return false;
  if (shell_.syncing || notification != BN_CLICKED) return true;
  if (id == IDC_ITEM_ADD || id == IDC_ITEM_EDIT) {
    open_panel_item_dialog(id == IDC_ITEM_EDIT);
    return true;
  }
  if (!draft_controls_valid() ||
      !apply_profile_controls(ChangeKind::text)) {
    set_status(L"Correct invalid profile fields before editing items.");
    return true;
  }
  const auto *panel = selected_bar();
  const auto row = SendDlgItemMessageW(
      shell_.window, IDC_PANEL_ITEMS, LB_GETCURSEL, 0, 0);
  if (panel == nullptr || row < 0 ||
      static_cast<std::size_t>(row) >= row_count(*panel)) return true;
  const auto panel_index =
      static_cast<std::size_t>(panel - document_.profile().panels.data());
  const auto index = static_cast<std::size_t>(row);
  if (id == IDC_ITEM_REMOVE) {
    const bool opaque = panel->items &&
        std::holds_alternative<core::OpaqueOverlayItem>((*panel->items)[index].content);
    const auto message = opaque
        ? L"Remove this unsupported item? Its preserved data will be deleted."
        : L"Remove the selected overlay item?";
    if (themed_message_box(shell_.window, message, L"Remove overlay item",
                           MB_YESNO | MB_ICONWARNING) != IDYES)
      return true;
  }
  const auto target = id == IDC_ITEM_UP
                          ? (index == 0 ? index : index - 1)
                          : std::min(index + 1, row_count(*panel) - 1);
  const auto outcome = document_.edit_profile_and_automation(
      [&](core::GameProfile &profile,
          std::optional<core::AutomationProfile> &automation)
          -> std::optional<std::string> {
        const auto failure = id == IDC_ITEM_REMOVE
            ? remove_panel_item(profile, automation, panel_index, index)
            : move_panel_item(profile, automation, panel_index, index, target);
        return failure ? std::optional<std::string>(failure->message)
                       : std::nullopt;
      }, ChangeKind::immediate);
  if (apply_edit(outcome, ChangeKind::immediate)) {
    refresh_bar_list(panel_index);
    refresh_panel_items(id == IDC_ITEM_REMOVE ? index : target);
    layout();
  }
  return true;
}

} // namespace layerc::editor
