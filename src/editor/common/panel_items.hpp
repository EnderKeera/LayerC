#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::editor {

using ItemIdSource = std::function<std::string()>;
[[nodiscard]] std::string make_uuid_item_id();
[[nodiscard]] std::vector<std::string> panel_cooldown_members(const core::Panel &);
[[nodiscard]] bool panel_has_cooldown(const core::Panel &, std::string_view);
[[nodiscard]] std::string item_display_name(const core::GameProfile &, const core::OverlayItem &);
[[nodiscard]] std::string item_kind_text(const core::OverlayItem &);

struct ItemEditError { std::string message; };
// Every structural edit is atomic, including legacy-row promotion and override
// ID remapping. IDs are generated only for new rows, never on display or save.
[[nodiscard]] std::optional<ItemEditError> promote_panel_items(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    const ItemIdSource &ids = make_uuid_item_id);
[[nodiscard]] std::optional<ItemEditError> add_panel_item(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    core::OverlayItem, const ItemIdSource &ids = make_uuid_item_id);
[[nodiscard]] std::optional<ItemEditError> update_panel_item(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    std::size_t index, core::OverlayItem, const ItemIdSource &ids = make_uuid_item_id);
[[nodiscard]] std::optional<ItemEditError> remove_panel_item(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    std::size_t index, const ItemIdSource &ids = make_uuid_item_id);
[[nodiscard]] std::optional<ItemEditError> move_panel_item(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    std::size_t from, std::size_t to, const ItemIdSource &ids = make_uuid_item_id);
struct TimerPresetResult { std::string timer_id; std::string automation_id; std::string item_id; };
[[nodiscard]] std::variant<TimerPresetResult, ItemEditError> add_automation_timer_preset(
    core::GameProfile &, std::optional<core::AutomationProfile> &, std::size_t panel,
    std::string_view cooldown_id, core::Milliseconds interval,
    std::optional<std::string> label, const ItemIdSource &ids = make_uuid_item_id);
// Only removed/renamed variable IDs are checked: unrelated edits never reject
// an already unavailable item. Disabled rules/items keep their dependencies.
[[nodiscard]] std::optional<std::string> item_variable_removal_error(
    const core::GameProfile &, const std::optional<core::AutomationProfile> &before,
    const std::optional<core::AutomationProfile> &after);

} // namespace layerc::editor
