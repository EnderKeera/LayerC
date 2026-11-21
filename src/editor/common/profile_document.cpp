#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "panel_items.hpp"
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
  const auto &visual = loaded.modules.get<layerc::core::VisualModule>();
  const auto &automation = loaded.modules.get<layerc::core::AutomationModule>();
  const bool blocked = unreadable(visual) || unreadable(automation);
  const bool items_upgrade = loaded.base_schema_below_items &&
      core::profile_serialization_schema(loaded.profile) >= core::profile_schema_version;
  return {.profile = std::move(loaded.profile),
          .visual = visual.optional_profile(),
          .automation = automation.optional_profile(),
          .unreadable_module = blocked,
          .upgrade_needed = loaded.legacy_base_schema || loaded.legacy_visual_schema || items_upgrade,
          .items_upgrade_needed = items_upgrade};
}

ModuleEditOutcome ProfileDocument::edit_automation_value(
    const std::function<std::optional<std::string>(
        std::optional<layerc::core::AutomationProfile> &)> &edit,
    const bool repair) {
  if (automation_state() == platform::ModuleSidecarState::invalid)
    return {ModuleEditStatus::blocked,
            "The unreadable automation module is preserved; editing is blocked."};
  auto candidate = automation_.profile;
  const bool was_invalid = repair && candidate &&
      !core::validate_automation_profile(*candidate, profile_).message.empty();
  if (const auto refusal = edit(candidate))
    return {ModuleEditStatus::refused, *refusal};
  if (candidate && !was_invalid) {
    const auto error = core::validate_automation_profile(*candidate, profile_);
    if (!error.message.empty())
      return {ModuleEditStatus::invalid, error.describe()};
  }
  if (const auto error = item_variable_removal_error(profile_, automation_.profile, candidate))
    return {ModuleEditStatus::refused, *error};
  if (candidate == automation_.profile) return {};
  automation_.profile = std::move(candidate);
  mark_automation_edited();
  return {ModuleEditStatus::accepted, {}};
}

ModuleEditOutcome ProfileDocument::edit_visual_value(
    const std::function<std::optional<std::string>(
        std::optional<layerc::core::VisualProfile> &)> &edit) {
  if (visual_state() == platform::ModuleSidecarState::invalid)
    return {ModuleEditStatus::blocked,
            "The unreadable visual module is preserved; recalibrate to repair it."};
  auto candidate = visual_.profile;
  if (const auto refusal = edit(candidate))
    return {ModuleEditStatus::refused, *refusal};
  if (candidate) {
    const auto error = core::validate_visual_profile(*candidate, profile_);
    if (!error.message.empty())
      return {ModuleEditStatus::invalid, error.describe()};
  }
  if (candidate == visual_.profile) return {};
  visual_.profile = std::move(candidate);
  mark_visual_edited();
  return {ModuleEditStatus::accepted, {}};
}

ModuleEditOutcome ProfileDocument::edit_profile_value(
    const std::function<void(layerc::core::GameProfile &)> &edit,
    const layerc::core::CompanionProfile *companion,
    const BeforeProfileAccept &before_accept) {
  auto candidate = profile_;
  edit(candidate);
  if (const auto error = core::validate_profile(candidate))
    return {ModuleEditStatus::invalid, profile_error_line(*error)};
  if (candidate == profile_) return {};
  if (visual_.profile && !core::VisualModule::valid(*visual_.profile, candidate))
    return {ModuleEditStatus::invalid,
            "The edit would invalidate the existing visual module."};
  if (automation_.profile &&
      !core::AutomationModule::valid(*automation_.profile, candidate))
    return {ModuleEditStatus::invalid,
            "The edit would invalidate the existing automation module."};
  if (companion && !core::CompanionModule::valid(*companion, candidate))
    return {ModuleEditStatus::invalid,
            "The edit would invalidate the read-only Game Bar companion module."};
  if (before_accept)
    if (auto refusal = before_accept())
      return {ModuleEditStatus::refused, std::move(*refusal)};
  profile_ = std::move(candidate);
  return {ModuleEditStatus::accepted, {}};
}

std::variant<ProfileBundle, EnablementRefusal> profile_enablement_bundle(
    const ProfileDocument &document, const bool is_current,
    const std::filesystem::path &target, const OpenDocumentEnablement policy) {
  if (is_current) {
    if (policy == OpenDocumentEnablement::commit_open_edits)
      return document.bundle();
    if (document.dirty()) return EnablementRefusal::dirty_open_document;
  }
  auto loaded = platform::load_profile_bundle(target);
  if (const auto *error = std::get_if<platform::ProfileBundleLoadError>(&loaded))
    return *error == platform::ProfileBundleLoadError::unreadable
               ? EnablementRefusal::unreadable
               : EnablementRefusal::invalid_profile;
  return loaded_bundle(std::move(std::get<platform::ProfileBundleLoad>(loaded)));
}

ModuleEditOutcome ProfileDocument::edit_profile_and_automation(
    const std::function<std::optional<std::string>(
        core::GameProfile &, std::optional<core::AutomationProfile> &)> &edit,
    const DocumentChangeKind kind) {
  if (!platform::may_write_primary(automation_.state))
    return {ModuleEditStatus::blocked, "Repair the automation module first."};
  auto profile = profile_;
  auto automation = automation_.profile;
  if (const auto error = edit(profile, automation))
    return {ModuleEditStatus::refused, *error};
  if (const auto error = core::validate_profile(profile))
    return {ModuleEditStatus::invalid, error->describe()};
  if (visual_.profile && !core::VisualModule::valid(*visual_.profile, profile))
    return {ModuleEditStatus::invalid, "The edit would invalidate the existing visual module."};
  if (automation) {
    const auto error = core::validate_automation_profile(*automation, profile);
    if (!error.message.empty()) return {ModuleEditStatus::invalid, error.describe()};
  }
  if (const auto error = item_variable_removal_error(profile, automation_.profile, automation))
    return {ModuleEditStatus::refused, *error};
  if (profile == profile_ && automation == automation_.profile) return {};
  if (kind != DocumentChangeKind::text) history_.end_text_coalescing();
  if (automation != automation_.profile) {
    automation_.profile = std::move(automation);
    mark_automation_edited();
  }
  profile_ = std::move(profile);
  (void)history_.record(profile_, visual_.profile, automation_.profile,
                        kind == DocumentChangeKind::text);
  return {ModuleEditStatus::accepted, {}};
}

void sync_bar_panels(layerc::core::GameProfile &profile,
                     const std::optional<std::string_view> added) {
  using namespace core;
  const bool independent = !profile.panels.empty() &&
      std::ranges::all_of(profile.panels, [](const Panel &panel) {
        return panel.style == PanelStyle::bar &&
            (panel.items ? panel.items->size() == 1 &&
                std::holds_alternative<CooldownItem>(panel.items->front().content)
                         : panel.cooldown_ids.size() == 1);
      });
  const auto exists = [&](const std::string_view id) {
    return std::ranges::any_of(profile.cooldowns, [&](const auto &cooldown) {
      return cooldown.id == id;
    });
  };
  for (auto &panel : profile.panels) {
    if (panel.items) {
      std::erase_if(*panel.items, [&](const auto &item) {
        if (const auto *source = std::get_if<CooldownItem>(&item.content))
          return !exists(source->cooldown_id);
        if (const auto *source = std::get_if<CooldownActiveTimerItem>(&item.content))
          return !exists(source->cooldown_id);
        return false;
      });
    } else {
      std::erase_if(panel.cooldown_ids, [&](const auto &id) { return !exists(id); });
    }
  }
  std::erase_if(profile.panels, [](const auto &panel) {
    return panel.items ? panel.items->empty() : panel.cooldown_ids.empty();
  });
  if (profile.cooldowns.empty()) return;
  if (added && independent) {
    const auto cooldown = std::ranges::find(profile.cooldowns, *added, &CooldownDefinition::id);
    if (cooldown != profile.cooldowns.end()) {
      Panel panel;
      panel.id = unique_panel_id(cooldown->id + "-bar", profile.panels);
      panel.name = cooldown->name;
      panel.style = PanelStyle::bar;
      panel.anchor_x = 0.5;
      panel.anchor_y = std::clamp(
          0.2 + 0.10 * static_cast<double>(profile.cooldowns.size() - 1U), 0.0, 1.0);
      panel.cooldown_ids = {cooldown->id};
      profile.panels.push_back(std::move(panel));
      return;
    }
  }
  if (profile.panels.empty())
    profile.panels.push_back({.id = "main", .name = "Main", .style = PanelStyle::bar,
                              .bar = {}, .cooldown_ids = {}});
  const std::string_view required = added.value_or(profile.cooldowns.front().id);
  if (std::ranges::none_of(profile.panels, [&](const auto &panel) {
        return panel_has_cooldown(panel, required);
      })) {
    auto &front = profile.panels.front();
    if (front.items)
      front.items->push_back({.id = make_uuid_item_id(), .label = std::nullopt,
                             .content = CooldownItem{std::string(required)}});
    else
      front.cooldown_ids.emplace_back(required);
  }
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
  std::vector<std::pair<std::string, std::string>> removed_items;
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
    std::size_t removed = 0;
    if (panel.items) {
      for (const auto &item : *panel.items) {
        const auto *cd = std::get_if<CooldownItem>(&item.content);
        const auto *active = std::get_if<CooldownActiveTimerItem>(&item.content);
        if ((cd && cd->cooldown_id == id) || (active && active->cooldown_id == id)) {
          removed_items.emplace_back(panel.id, item.id);
          ++removed;
        }
      }
    } else {
      for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index)
        if (panel.cooldown_ids[index] == id) {
          removed_items.emplace_back(panel.id, "@legacy:" + std::to_string(index));
          ++removed;
        }
    }
    const auto count = panel.items ? panel.items->size() : panel.cooldown_ids.size();
    if (removed != 0 && removed == count) removed_panels.push_back(panel.id);
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
          const bool item = action.kind == AutomationActionKind::set_overlay_override &&
              std::ranges::any_of(removed_items, [&](const auto &removed) {
                return action.target == removed.first && action.item_id == removed.second;
              });
          if (direct || group || panel || item)
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
  auto automation = automation_.profile;
  // Legacy item aliases are positional. Promote an affected panel before
  // deleting a row so surviving overrides retain their original identity.
  if (automation) {
    for (std::size_t index = 0; index < candidate.panels.size(); ++index) {
      const auto &panel = candidate.panels[index];
      if (panel.items || !panel_has_cooldown(panel, removed_id)) continue;
      const bool item_override = std::ranges::any_of(automation->automations, [&](const auto &a) {
        return std::ranges::any_of(a.rules, [&](const auto &rule) {
          return std::ranges::any_of(rule.actions, [&](const auto &action) {
            return action.kind == core::AutomationActionKind::set_overlay_override &&
                   action.target == panel.id && !action.item_id.empty();
          });
        });
      });
      if (item_override)
        if (const auto error = promote_panel_items(candidate, automation, index))
          return CooldownRemovalError{error->message};
    }
  }
  std::erase_if(candidate.cooldowns,
                [&](const auto &cooldown) { return cooldown.id == removed_id; });
  std::erase_if(candidate.bindings, [&](const auto &binding) {
    return binding.cooldown_id == removed_id;
  });
  layerc::core::compact_hid_bindings(candidate);
  sync_bar_panels(candidate);
  if (visual)
    erase_visual_assignment(*visual, removed_id);

  if (const auto error = layerc::core::validate_profile(candidate))
    return CooldownRemovalError{error->describe()};
  if (visual) {
    const auto error = layerc::core::validate_visual_profile(*visual, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.describe()};
  }
  if (automation) {
    const auto error = layerc::core::validate_automation_profile(
        *automation, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.describe()};
  }

  profile_ = std::move(candidate);
  if (automation != automation_.profile) {
    automation_.profile = std::move(automation);
    mark_automation_edited();
  }
  if (visual_.profile) {
    visual_.profile = std::move(visual);
    mark_visual_edited();
  }
  // One accepted removal is one undoable step.
  (void)history_.record(profile_, visual_.profile, automation_.profile);
  if (path_.empty()) history_.forget_saved();
  return std::nullopt;
}

// ---- F01 transitions ---------------------------------------------------------

void ProfileDocument::open(std::filesystem::path path,
                           layerc::platform::ProfileBundleLoad loaded) {
  profile_ = std::move(loaded.profile);
  path_ = std::move(path);
  const auto &visual = loaded.modules.get<layerc::core::VisualModule>();
  set_visual(visual, loaded.legacy_visual_schema);
  set_automation(loaded.modules.get<layerc::core::AutomationModule>());
  legacy_base_schema_ = loaded.legacy_base_schema;
  base_schema_below_items_ = loaded.base_schema_below_items;
  items_upgrade_acknowledged_ = false;
  upgrade_acknowledged_ = false;
  // Retain an unreadable visual sidecar so undo can restore exactly its state
  // and diagnostic after an explicit calibration repair.
  retained_visual_ = visual.state() == platform::ModuleSidecarState::invalid
                         ? std::optional<layerc::platform::VisualSidecarLoad>(visual)
                         : std::nullopt;
  history_.reset(profile_, visual_.profile, automation_.profile);
}

void ProfileDocument::start_draft(
    layerc::core::GameProfile profile, const DraftOrigin origin,
    std::optional<layerc::core::VisualProfile> visual,
    std::optional<layerc::core::AutomationProfile> automation) {
  profile_ = std::move(profile);
  path_.clear();
  legacy_base_schema_ = false;
  legacy_visual_schema_ = false;
  base_schema_below_items_ = false;
  items_upgrade_acknowledged_ = false;
  upgrade_acknowledged_ = false;
  retained_visual_.reset();
  visual_.profile = std::move(visual);
  visual_.mark_edited();
  visual_.diagnostic.clear();
  automation_.profile = std::move(automation);
  automation_.mark_edited();
  automation_.diagnostic.clear();
  // A copy has never been committed: no saved snapshot, dirty by construction.
  history_.reset(profile_, visual_.profile, automation_.profile,
                 origin == DraftOrigin::fresh);
}

void ProfileDocument::close() { *this = ProfileDocument{}; }

void ProfileDocument::accept_commit(std::filesystem::path path,
                                    ProfileBundle committed,
                                    const CommitRecord record) {
  const bool changed = committed.profile != profile_ ||
                       committed.visual != visual_.profile ||
                       committed.automation != automation_.profile;
  // A candidate commit is one undoable step; a plain save only rebases.
  if (record == CommitRecord::record_edit && changed)
    (void)history_.record(committed.profile, committed.visual,
                          committed.automation);
  if (committed.visual != visual_.profile) {
    visual_.profile = std::move(committed.visual);
    mark_visual_edited();
    visual_.diagnostic.clear();
  }
  if (committed.automation != automation_.profile) {
    automation_.profile = std::move(committed.automation);
    mark_automation_edited();
    automation_.diagnostic.clear();
  }
  profile_ = std::move(committed.profile);
  path_ = std::move(path);
  legacy_base_schema_ = false;
  legacy_visual_schema_ = false;
  base_schema_below_items_ = core::profile_serialization_schema(profile_) < core::profile_schema_version;
  items_upgrade_acknowledged_ = false;
  upgrade_acknowledged_ = false;
  retained_visual_.reset();
  history_.rebase(profile_, visual_.profile, automation_.profile);
}

ModuleEditOutcome ProfileDocument::edit_profile(
    const std::function<void(layerc::core::GameProfile &)> &edit,
    const DocumentChangeKind kind,
    const layerc::core::CompanionProfile *companion,
    const BeforeProfileAccept &before_accept) {
  const auto outcome = edit_profile_value(edit, companion, before_accept);
  if (outcome.accepted())
    (void)history_.record(profile_, visual_.profile, automation_.profile,
                          kind == DocumentChangeKind::text);
  if (outcome.accepted() && path_.empty()) history_.forget_saved();
  return outcome;
}

ModuleEditOutcome ProfileDocument::edit_visual(
    const std::function<std::optional<std::string>(
        std::optional<layerc::core::VisualProfile> &)> &edit,
    const DocumentChangeKind kind) {
  const auto outcome = edit_visual_value(edit);
  if (outcome.accepted())
    (void)history_.record(profile_, visual_.profile, automation_.profile,
                          kind == DocumentChangeKind::text);
  if (outcome.accepted() && path_.empty()) history_.forget_saved();
  return outcome;
}

ModuleEditOutcome ProfileDocument::edit_automation(
    const std::function<std::optional<std::string>(
        std::optional<layerc::core::AutomationProfile> &)> &edit,
    const DocumentChangeKind kind, const bool repair) {
  const auto outcome = edit_automation_value(edit, repair);
  if (outcome.accepted())
    (void)history_.record(profile_, visual_.profile, automation_.profile,
                          kind == DocumentChangeKind::text);
  if (outcome.accepted() && path_.empty()) history_.forget_saved();
  return outcome;
}

ModuleEditOutcome ProfileDocument::replace_visual_from_calibration(
    layerc::core::VisualProfile visual) {
  const auto error = layerc::core::validate_visual_profile(visual, profile_);
  if (!error.message.empty())
    return {ModuleEditStatus::invalid, error.describe()};
  if (visual == visual_.profile &&
      visual_state() != platform::ModuleSidecarState::invalid)
    return {};
  visual_.profile = std::move(visual);
  mark_visual_edited();
  visual_.diagnostic.clear();
  (void)history_.record(profile_, visual_.profile, automation_.profile);
  if (path_.empty()) history_.forget_saved();
  return {ModuleEditStatus::accepted, {}};
}

bool ProfileDocument::undo() {
  auto snapshot = history_.undo(profile_, visual_.profile, automation_.profile);
  if (!snapshot) return false;
  profile_ = std::move(snapshot->profile);
  if (snapshot->visual_profile != visual_.profile) {
    if (!snapshot->visual_profile && retained_visual_ &&
        retained_visual_->state() == platform::ModuleSidecarState::invalid) {
      set_visual(*retained_visual_, legacy_visual_schema_);
    } else {
      visual_.profile = std::move(snapshot->visual_profile);
      mark_visual_edited();
      visual_.diagnostic.clear();
    }
  }
  if (snapshot->automation_profile != automation_.profile) {
    automation_.profile = std::move(snapshot->automation_profile);
    mark_automation_edited();
    automation_.diagnostic.clear();
  }
  return true;
}

bool ProfileDocument::redo() {
  auto snapshot = history_.redo(profile_, visual_.profile, automation_.profile);
  if (!snapshot) return false;
  profile_ = std::move(snapshot->profile);
  if (snapshot->visual_profile != visual_.profile) {
    if (!snapshot->visual_profile && retained_visual_ &&
        retained_visual_->state() == platform::ModuleSidecarState::invalid) {
      set_visual(*retained_visual_, legacy_visual_schema_);
    } else {
      visual_.profile = std::move(snapshot->visual_profile);
      mark_visual_edited();
      visual_.diagnostic.clear();
    }
  }
  if (snapshot->automation_profile != automation_.profile) {
    automation_.profile = std::move(snapshot->automation_profile);
    mark_automation_edited();
    automation_.diagnostic.clear();
  }
  return true;
}

} // namespace layerc::editor
