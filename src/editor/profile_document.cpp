#include "profile_document.hpp"
#include "editor_util.hpp"
#include "layerc/core/hid.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::editor {

ProfileBundle loaded_bundle(layerc::platform::ProfileBundleLoad loaded) {
  const auto unreadable = [](const auto &sidecar) {
    return sidecar.state() == layerc::platform::ModuleSidecarState::invalid &&
           !sidecar.optional_profile().has_value();
  };
  const bool blocked = unreadable(loaded.visual) || unreadable(loaded.automation);
  return {.profile = std::move(loaded.profile),
          .visual = std::move(loaded.visual.optional_profile()),
          .automation = std::move(loaded.automation.optional_profile()),
          .unreadable_module = blocked,
          .upgrade_needed =
              loaded.legacy_base_schema || loaded.legacy_visual_schema};
}

void sync_bar_panels(layerc::core::GameProfile &profile,
                     const std::optional<std::string_view> added) {
  const bool independent =
      !profile.panels.empty() &&
      std::ranges::all_of(profile.panels, [](const layerc::core::Panel &panel) {
        return panel.style == layerc::core::PanelStyle::bar &&
               panel.cooldown_ids.size() == 1;
      });
  const auto exists = [&](const std::string_view id) {
    return std::ranges::any_of(profile.cooldowns, [&](const auto &cooldown) {
      return cooldown.id == id;
    });
  };
  for (auto &panel : profile.panels)
    std::erase_if(panel.cooldown_ids,
                  [&](const auto &id) { return !exists(id); });
  std::erase_if(profile.panels,
                [](const auto &panel) { return panel.cooldown_ids.empty(); });
  if (profile.cooldowns.empty())
    return;

  if (added && independent) {
    const auto cooldown = std::ranges::find(
        profile.cooldowns, *added, &layerc::core::CooldownDefinition::id);
    if (cooldown != profile.cooldowns.end()) {
      layerc::core::Panel panel;
      panel.id = unique_panel_id(cooldown->id + "-bar", profile.panels);
      panel.name = cooldown->name;
      panel.style = layerc::core::PanelStyle::bar;
      panel.anchor_x = 0.5;
      panel.anchor_y = std::clamp(
          0.2 + 0.10 * static_cast<double>(profile.cooldowns.size() - 1U), 0.0,
          1.0);
      panel.cooldown_ids = {cooldown->id};
      profile.panels.push_back(std::move(panel));
      return;
    }
  }

  if (profile.panels.empty()) {
    profile.panels.push_back({.id = "main",
                              .name = "Main",
                              .style = layerc::core::PanelStyle::bar,
                              .bar = {},
                              .cooldown_ids = {}});
  }
  const std::string_view required =
      added.value_or(profile.cooldowns.front().id);
  const bool assigned = std::ranges::any_of(
      profile.panels, [&](const layerc::core::Panel &panel) {
        return std::ranges::find(panel.cooldown_ids, required) !=
               panel.cooldown_ids.end();
      });
  if (!assigned)
    profile.panels.front().cooldown_ids.emplace_back(required);
}

std::optional<CooldownRemovalError>
ProfileDocument::cooldown_removal_error(const std::string_view id) const {
  using namespace layerc::core;
  if (std::ranges::find(profile_.cooldowns, id, &CooldownDefinition::id) ==
      profile_.cooldowns.end())
    return CooldownRemovalError{"The selected cooldown no longer exists."};
  if (profile_.cooldowns.size() <= 1)
    return CooldownRemovalError{"A profile must retain at least one cooldown."};
  if (!layerc::platform::may_write_primary(visual_.state) ||
      !layerc::platform::may_write_primary(automation_.state))
    return CooldownRemovalError{
        "Repair the recovered or invalid sidecar before removing a cooldown."};

  std::vector<std::string_view> affected_groups;
  std::vector<std::string_view> removed_panels;
  std::string dependencies;
  for (const auto &group : profile_.cooldown_groups) {
    if (std::ranges::find(group.cooldown_ids, id) == group.cooldown_ids.end())
      continue;
    affected_groups.push_back(group.id);
    dependencies += "\n- Cooldown group '" + group.id + "'";
    if (!group.name.empty())
      dependencies += " (" + group.name + ")";
  }
  for (const auto &panel : profile_.panels) {
    if (panel.cooldown_ids.size() == 1 && panel.cooldown_ids.front() == id)
      removed_panels.push_back(panel.id);
  }
  auto remaining_visual = visual_.profile;
  if (remaining_visual)
    erase_visual_assignment(*remaining_visual, id);
  if (automation_.profile) {
    for (const auto &automation : automation_.profile->automations) {
      // Disabled rules still own their references and must remain re-enableable.
      for (const auto &rule : automation.rules) {
        const bool cooldown_source =
            rule.source_filter == id &&
            (rule.event == AutomationEventKind::cooldown_triggered ||
             rule.event == AutomationEventKind::cooldown_ready);
        if (cooldown_source)
          dependencies += "\n- Automation '" + automation.id + "', rule '" +
                          rule.id + "': cooldown event source '" +
                          rule.source_filter + "'";
        if (rule.event == AutomationEventKind::visual_observation &&
            visual_.profile && remaining_visual &&
            find_observation(*visual_.profile, rule.source_filter) &&
            !find_observation(*remaining_visual, rule.source_filter))
          dependencies += "\n- Automation '" + automation.id + "', rule '" +
                          rule.id + "': visual observation '" +
                          rule.source_filter + "'";
        for (const auto &action : rule.actions) {
          const auto *spec = automation_action_spec(action.kind);
          if (spec == nullptr)
            continue;
          const bool direct = spec->target == ActionTargetRole::cooldown &&
                              action.target == id;
          const bool group =
              spec->target == ActionTargetRole::cooldown_group &&
              std::ranges::find(affected_groups, action.target) !=
                  affected_groups.end();
          const bool panel =
              spec->target == ActionTargetRole::panel &&
              std::ranges::find(removed_panels, action.target) !=
                  removed_panels.end();
          if (direct || group || panel)
            dependencies += "\n- Automation '" + automation.id + "', rule '" +
                            rule.id + "': " + std::string(spec->wire_name) +
                            " target '" + action.target + "'";
        }
      }
    }
  }
  if (!dependencies.empty())
    return CooldownRemovalError{"Cannot remove '" + std::string(id) +
                                "'. Remove these dependencies first:" +
                                dependencies};
  return std::nullopt;
}

std::optional<CooldownRemovalError>
ProfileDocument::remove_cooldown(const std::string_view id) {
  if (const auto error = cooldown_removal_error(id))
    return error;

  // Keep rejected edits completely inert, including dirty/history and sidecar
  // recovery metadata. The string copy also permits an id borrowed from profile_.
  const std::string removed_id(id);
  auto candidate = profile_;
  auto visual = visual_.profile;
  std::erase_if(candidate.cooldowns,
                [&](const auto &cooldown) { return cooldown.id == removed_id; });
  std::erase_if(candidate.bindings, [&](const auto &binding) {
    return binding.cooldown_id == removed_id;
  });
  layerc::core::compact_hid_bindings(candidate);
  sync_bar_panels(candidate);
  if (visual)
    erase_visual_assignment(*visual, removed_id);

  const auto parsed = layerc::core::parse_profile_json(
      layerc::core::serialize_profile_json(candidate));
  if (const auto *error = std::get_if<layerc::core::ProfileError>(&parsed))
    return CooldownRemovalError{error->path + ": " + error->message};
  if (visual) {
    const auto error = layerc::core::validate_visual_profile(*visual, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.path + ": " + error.message};
  }
  if (automation_.profile) {
    const auto error = layerc::core::validate_automation_profile(
        *automation_.profile, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.path + ": " + error.message};
  }

  profile_ = std::move(candidate);
  if (visual_.profile) {
    visual_.profile = std::move(visual);
    mark_visual_edited();
  }
  dirty_ = true;
  return std::nullopt;
}

} // namespace layerc::editor
