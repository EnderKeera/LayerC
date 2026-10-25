#include "profile_document.hpp"
#include "profile_edit.hpp"
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
  return {.profile = std::move(loaded.profile),
          .visual = visual.optional_profile(),
          .automation = automation.optional_profile(),
          .unreadable_module = blocked,
          .upgrade_needed =
              loaded.legacy_base_schema || loaded.legacy_visual_schema};
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
    const layerc::core::CompanionProfile *companion) {
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

  if (const auto error = layerc::core::validate_profile(candidate))
    return CooldownRemovalError{error->describe()};
  if (visual) {
    const auto error = layerc::core::validate_visual_profile(*visual, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.describe()};
  }
  if (automation_.profile) {
    const auto error = layerc::core::validate_automation_profile(
        *automation_.profile, candidate);
    if (!error.message.empty())
      return CooldownRemovalError{error.describe()};
  }

  profile_ = std::move(candidate);
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
  upgrade_acknowledged_ = false;
  retained_visual_.reset();
  history_.rebase(profile_, visual_.profile, automation_.profile);
}

ModuleEditOutcome ProfileDocument::edit_profile(
    const std::function<void(layerc::core::GameProfile &)> &edit,
    const DocumentChangeKind kind,
    const layerc::core::CompanionProfile *companion) {
  const auto outcome = edit_profile_value(edit, companion);
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
