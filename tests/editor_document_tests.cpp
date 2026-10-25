#include "diagnostic_text.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace layerc::core;
using layerc::editor::DocumentChangeKind;
using layerc::editor::DraftOrigin;
using layerc::editor::ProfileDocument;
using layerc::test::expect;

ProfileDocument fixture() {
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  auto other = profile.cooldowns.front();
  other.id = "other";
  other.name = "Other";
  profile.cooldowns.push_back(std::move(other));
  profile.panels.front().cooldown_ids.push_back("other");
  profile.bindings.push_back(
      {.cooldown_id = "other",
       .chord = InputChord{{{InputDevice::keyboard, 0x13, 0}}}});
  VisualProfile visual;
  visual.profile_id = profile.id;
  visual.observations.push_back(PixelRule{.id = "ready", .name = "Ready"});
  visual.triggers.push_back({.cooldown_id = "ability", .observation_id = "ready"});
  ProfileDocument document;
  document.start_draft(std::move(profile), DraftOrigin::fresh,
                       std::move(visual));
  return document;
}

void add_rule(ProfileDocument &document, AutomationAction action,
              const bool enabled = true) {
  AutomationProfile automation;
  automation.profile_id = document.profile().id;
  AutomationRule rule;
  rule.id = "fixture-rule";
  rule.event = AutomationEventKind::session_started;
  rule.actions.push_back(std::move(action));
  automation.automations.push_back(
      {.id = "fixture-automation", .name = "Fixture", .enabled = enabled,
       .rules = {std::move(rule)}});
  // Fixture assembly only: some supplied actions stay deliberately incomplete
  // until the caller's local setup, so nothing is validated here.
  document.start_draft(document.profile(), DraftOrigin::fresh, document.visual(),
                       std::move(automation));
}

layerc::platform::ProfileBundleLoad fixture_load(
    GameProfile profile, std::optional<VisualProfile> visual = {},
    std::optional<AutomationProfile> automation = {}) {
  layerc::platform::ProfileBundleLoad loaded;
  loaded.profile = std::move(profile);
  if (visual)
    loaded.modules.get<VisualModule>() =
        layerc::platform::VisualSidecarLoad::valid(std::move(*visual), {});
  if (automation)
    loaded.modules.get<AutomationModule>() =
        layerc::platform::AutomationSidecarLoad::valid(std::move(*automation), {});
  return loaded;
}

void expect_valid(const ProfileDocument &document) {
  const auto base_error = validate_profile(document.profile());
  expect(!base_error, base_error ? base_error->describe() : std::string{});
  if (document.visual()) {
    const auto error =
        validate_visual_profile(*document.visual(), document.profile());
    expect(error.message.empty(), error.describe());
  }
  if (document.automation()) {
    const auto error =
        validate_automation_profile(*document.automation(), document.profile());
    expect(error.message.empty(), error.describe());
  }
}

void expect_rejected(ProfileDocument &document, const std::string &dependency) {
  const layerc::editor::EditorProfileSnapshot before{
      document.profile(), document.visual(), document.automation()};
  const auto visual_state = document.visual_state();
  const auto automation_state = document.automation_state();
  const bool dirty = document.dirty();
  const bool can_undo = document.history().can_undo();
  const bool can_redo = document.history().can_redo();
  const auto error = document.remove_cooldown("ability");
  expect(error && error->message.find(dependency) != std::string::npos,
         "a rejected deletion must identify its exact dependency");
  expect(document.profile() == before.profile &&
             document.visual() == before.visual_profile &&
             document.automation() == before.automation_profile &&
             document.visual_state() == visual_state &&
             document.automation_state() == automation_state &&
             document.dirty() == dirty &&
             document.history().can_undo() == can_undo &&
             document.history().can_redo() == can_redo,
         "rejected deletion must not mutate the bundle, metadata or history");
}
// Each fixture family is a separate function so /analyze bounds the stack of
// every case independently (C6262).
void group_removal_cases() {
  for (const auto mode : {CooldownGroupMode::shared_pool,
                          CooldownGroupMode::trigger_propagation}) {
    auto document = fixture();
    auto seeded = document.profile();
    seeded.cooldown_groups.push_back(
        {.id = "fixture-group", .name = "Fixture group", .mode = mode,
         .cooldown_ids = {"ability", "other"},
         .duration = std::chrono::seconds(5)});
    document.start_draft(std::move(seeded), DraftOrigin::fresh,
                         document.visual(), document.automation());
    expect_valid(document);
    expect_rejected(document, "Cooldown group 'fixture-group'");
    add_rule(document, {.kind = AutomationActionKind::trigger_group,
                        .target = "fixture-group"});
    expect_valid(document);
    const auto error = document.cooldown_removal_error("ability");
    expect(error && error->message.find("fixture-group") != std::string::npos &&
               error->message.find("fixture-rule") != std::string::npos,
           "preflight must report both group membership and affected-group rules");
    expect_rejected(document, "fixture-group");
  }
}

void automation_target_cases() {
  for (const auto &spec : automation_action_specs) {
    if (spec.target != ActionTargetRole::cooldown)
      continue;
    auto document = fixture();
    AutomationAction action{.kind = spec.kind, .target = "ability"};
    if (spec.accepts_value())
      action.value = expr::literal(AutomationValue(std::int64_t{1}));
    add_rule(document, std::move(action), false);
    expect_valid(document);
    expect_rejected(document, std::string(spec.wire_name));
  }
}

void automation_source_cases() {
  {
    auto document = fixture();
    add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                        .target = "other"});
    auto edited = *document.automation();
    edited.automations.front().rules.front().event =
        AutomationEventKind::cooldown_ready;
    edited.automations.front().rules.front().source_filter = "ability";
    document.start_draft(document.profile(), DraftOrigin::fresh,
                         document.visual(), std::move(edited));
    expect_valid(document);
    expect_rejected(document, "cooldown event source 'ability'");
  }

  {
    auto document = fixture();
    add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                        .target = "other"});
    auto edited = *document.automation();
    edited.automations.front().rules.front().event =
        AutomationEventKind::visual_observation;
    edited.automations.front().rules.front().source_filter = "ready";
    document.start_draft(document.profile(), DraftOrigin::fresh,
                         document.visual(), std::move(edited));
    expect_valid(document);
    expect_rejected(document, "visual observation 'ready'");
  }

  {
    auto document = fixture();
    auto seeded = document.profile();
    seeded.panels.front().id = "ability-bar";
    seeded.panels.front().cooldown_ids = {"ability"};
    auto other = seeded.panels.front();
    other.id = "other-bar";
    other.cooldown_ids = {"other"};
    seeded.panels.push_back(std::move(other));
    document.start_draft(std::move(seeded), DraftOrigin::fresh,
                         document.visual(), document.automation());
    add_rule(document, {.kind = AutomationActionKind::set_overlay_override,
                        .target = "ability-bar", .name = "fixture-override",
                        .property = "visible",
                        .value = expr::literal(AutomationValue(true))});
    expect_valid(document);
    expect_rejected(document, "ability-bar");
  }
}

void accepted_removal_case() {
  auto document = fixture();
  {
    add_rule(document, {.kind = AutomationActionKind::set_variable,
                        .target = "ability",
                        .value = expr::literal(AutomationValue(std::int64_t{1}))});
    auto edited = *document.automation();
    edited.variables.push_back(
        {.id = "ability", .type = AutomationValueType::integer,
         .default_value = AutomationValue(std::int64_t{0})});
    edited.automations.front().rules.front().event = AutomationEventKind::timer_fired;
    edited.automations.front().rules.front().source_filter = "ability";
    document.start_draft(document.profile(), DraftOrigin::fresh,
                         document.visual(), std::move(edited));
    expect_valid(document);
    const layerc::editor::EditorProfileSnapshot before{
        document.profile(), document.visual(), document.automation()};
    expect(!document.remove_cooldown("ability"),
           "unrelated variable/timer IDs must not block an unreferenced cooldown");
    expect(document.dirty() && document.profile().cooldowns.size() == 1 &&
               document.profile().bindings.size() == 1 &&
               document.visual()->triggers.empty(),
           "accepted deletion must remove the cooldown and its owned references");
    expect_valid(document);
    expect(document.can_undo() && document.dirty(),
           "accepted deletion records exactly one complete history transaction");
    expect(document.undo() &&
               layerc::editor::EditorProfileSnapshot{
                   document.profile(), document.visual(), document.automation()} ==
                   before &&
               !document.can_undo(),
           "one undo must restore the complete original bundle");
    expect_valid(document);
    expect(document.redo() && document.profile().cooldowns.size() == 1 &&
               document.visual()->triggers.empty() && !document.can_redo(),
           "one redo must restore the complete accepted removal");
    expect_valid(document);
  }
}

void invalid_bundle_cases() {
  {
    auto document = fixture();
    auto malformed = document.profile();
    malformed.cooldowns.back().duration = std::chrono::milliseconds(0);
    document.start_draft(std::move(malformed), DraftOrigin::copy,
                         document.visual(), document.automation());
    expect_rejected(document, "durationMs");
  }

  {
    auto document = fixture();
    add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                        .target = "ability"});
    expect(document.edit_profile([](auto &candidate) {
             candidate.name += " edited";
           }, DocumentChangeKind::immediate).accepted(),
           "the dependency refusal fixture carries one real recorded edit");
    expect_rejected(document, "fixture-rule");
  }

  {
    auto seeded = fixture();
    auto loaded = fixture_load(seeded.profile(), seeded.visual());
    loaded.modules.get<AutomationModule>() =
        layerc::platform::AutomationSidecarLoad::invalid(
            {.path = "$", .message = "fixture invalid sidecar"}, {});
    ProfileDocument document;
    document.open({}, std::move(loaded));
    expect_rejected(document, "sidecar");
  }
}

// The document and a loaded row build the same commit input: an unreadable
// module blocks, a usable one becomes a replacement, and legacy schemas need
// the upgrade confirmation until it is acknowledged.
void commit_bundle_cases() {
  {
    auto document = fixture();
    const auto bundle = document.bundle();
    expect(!bundle.unreadable_module && !bundle.upgrade_needed &&
               bundle.profile == document.profile() &&
               bundle.visual == document.visual() && !bundle.automation,
           "a valid document commits its profile and modules as-is");
  }
  {
    auto seeded = fixture();
    auto loaded = fixture_load(seeded.profile(), seeded.visual());
    loaded.modules.get<AutomationModule>() =
        layerc::platform::AutomationSidecarLoad::invalid(
            {.path = "$", .message = "fixture invalid sidecar"}, {});
    ProfileDocument document;
    document.open({}, std::move(loaded));
    expect(document.has_unreadable_module() &&
               document.bundle().unreadable_module,
           "an unreadable automation module blocks the document commit");
  }
  {
    auto document = fixture();
    layerc::platform::ProfileBundleLoad loaded{
        .profile = document.profile(),
        .legacy_base_schema = true};
    loaded.modules.get<layerc::core::VisualModule>() =
        layerc::platform::VisualSidecarLoad::invalid(layerc::core::VisualError{}, {});
    const auto bundle = layerc::editor::loaded_bundle(std::move(loaded));
    expect(bundle.unreadable_module && bundle.upgrade_needed &&
               !bundle.visual && !bundle.automation,
           "a loaded row with an unreadable visual module blocks its commit");
  }
}

void automation_candidate_contracts() {
  using layerc::editor::ModuleEditStatus;
  using layerc::platform::AutomationSidecarLoad;
  using layerc::platform::ModuleSidecarState;
  auto seeded = fixture();
  add_rule(seeded, {.kind = AutomationActionKind::trigger_cooldown,
                    .target = "ability"});
  auto loaded = fixture_load(seeded.profile(), seeded.visual());
  loaded.modules.get<layerc::core::AutomationModule>() =
      AutomationSidecarLoad::recovered(
          *seeded.automation(), {.path = "$", .message = "damaged primary"}, {});
  ProfileDocument document;
  document.open({}, std::move(loaded));
  const auto before = document.automation();
  const auto refused = document.edit_automation([](auto &candidate)
      -> std::optional<std::string> {
    candidate.reset();
    return "Keep the original graph.";
  }, DocumentChangeKind::immediate);
  expect(refused.status == ModuleEditStatus::refused &&
             refused.message == "Keep the original graph." &&
             document.automation() == before &&
             document.automation_state() == ModuleSidecarState::recovered_backup &&
             !document.history().can_undo() && !document.dirty(),
         "a refused candidate preserves graph, recovery state, history and dirty state");
  const auto invalid = document.edit_automation([](auto &candidate)
      -> std::optional<std::string> {
    candidate->profile_id = "wrong-profile";
    return std::nullopt;
  }, DocumentChangeKind::immediate);
  expect(invalid.status == ModuleEditStatus::invalid && !invalid.message.empty() &&
             document.automation() == before && !document.history().can_undo(),
         "an invalid candidate reports validation without changing the graph or history");
  const auto unchanged = document.edit_automation([](auto &)
      -> std::optional<std::string> { return std::nullopt; },
      DocumentChangeKind::immediate);
  expect(unchanged.status == ModuleEditStatus::unchanged && unchanged.message.empty() &&
             document.automation_state() == ModuleSidecarState::recovered_backup &&
             !document.history().can_undo(),
         "a no-op does not mark a recovered module edited or record history");
  const auto accepted = document.edit_automation([](auto &candidate)
      -> std::optional<std::string> {
    candidate->automations.front().name = "Accepted candidate";
    return std::nullopt;
  }, DocumentChangeKind::immediate);
  expect(accepted.accepted() && accepted.message.empty() &&
             document.automation()->automations.front().name == "Accepted candidate" &&
             document.automation_state() == ModuleSidecarState::valid &&
             document.can_undo() && document.dirty(),
         "an accepted automation edit is one undo step and dirty");
  auto unreadable = fixture_load(document.profile(), document.visual());
  unreadable.modules.get<layerc::core::AutomationModule>() =
      AutomationSidecarLoad::invalid({.path = "$", .message = "unreadable"}, {});
  document.open({}, std::move(unreadable));
  // Opening resets history, so the real accepted BASE rename comes after it:
  // the blocked probe must compare against nonzero pre-existing history.
  expect(document.edit_profile([](auto &candidate) {
           candidate.name += " history";
         }, DocumentChangeKind::immediate).accepted(),
         "a recorded base rename establishes nonzero history before the block");
  auto depth_probe = document;
  int undo_depth{};
  while (depth_probe.undo()) ++undo_depth;
  bool called{};
  const auto blocked = document.edit_automation([&](auto &)
      -> std::optional<std::string> { called = true; return std::nullopt; },
      DocumentChangeKind::immediate);
  auto after_blocked = document;
  int after_depth{};
  while (after_blocked.undo()) ++after_depth;
  expect(blocked.status == ModuleEditStatus::blocked && !called &&
             blocked.message ==
                 "The unreadable automation module is preserved; editing is blocked." &&
             !document.automation() && undo_depth > 0 && after_depth == undo_depth,
         "unreadable automation is blocked before the callback executes and adds no step");
}

void automation_repair_contracts() {
  using layerc::editor::ModuleEditStatus;
  auto document = fixture();
  add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                      .target = "ability"});
  // Initial plain fixture values: an unaligned module id and a second automation.
  auto seeded = *document.automation();
  seeded.profile_id = "wrong-profile";
  auto other = seeded.automations.front();
  other.id = "other-automation";
  seeded.automations.push_back(std::move(other));
  document.start_draft(document.profile(), DraftOrigin::fresh, document.visual(),
                       std::move(seeded));
  const auto before = document.automation();
  const auto removal = [](auto &candidate) -> std::optional<std::string> {
    candidate->automations.pop_back();
    return std::nullopt;
  };
  const auto rejected = document.edit_automation(removal, DocumentChangeKind::immediate);
  expect(rejected.status == ModuleEditStatus::invalid && !rejected.message.empty() &&
             document.automation() == before && !document.history().can_undo(),
         "ordinary edits cannot leave an invalid graph behind");
  const auto repaired = document.edit_automation(
      removal, layerc::editor::DocumentChangeKind::immediate, true);
  expect(repaired.accepted() && document.automation()->automations.size() == 1 &&
             !validate_automation_profile(*document.automation(), document.profile()).message.empty() &&
             document.can_undo() && document.dirty(),
         "an accepted repair removal is one undo step and dirty");
}

void visual_candidate_contracts() {
  using layerc::editor::ModuleEditStatus;
  auto document = fixture();
  const auto before = document.visual();
  const auto refused = document.edit_visual([](auto &candidate)
      -> std::optional<std::string> { candidate.reset(); return "Keep visual."; },
      DocumentChangeKind::immediate);
  expect(refused.status == ModuleEditStatus::refused && refused.message == "Keep visual." &&
             document.visual() == before && !document.history().can_undo(),
         "refused visual edits preserve values and history");
  const auto invalid = document.edit_visual([](auto &candidate)
      -> std::optional<std::string> {
    candidate->profile_id = "wrong-profile";
    return std::nullopt;
  }, DocumentChangeKind::immediate);
  expect(invalid.status == ModuleEditStatus::invalid && !invalid.message.empty() &&
             document.visual() == before && !document.history().can_undo(),
         "invalid visual edits preserve values and history");
  auto recovered = fixture_load(document.profile());
  recovered.modules.get<layerc::core::VisualModule>() =
      layerc::platform::VisualSidecarLoad::recovered(
          *before, {.path = "$", .message = "damaged primary"}, {});
  document.open({}, std::move(recovered));
  const auto unchanged = document.edit_visual([](auto &)
      -> std::optional<std::string> { return std::nullopt; },
      DocumentChangeKind::immediate);
  expect(unchanged.status == ModuleEditStatus::unchanged && unchanged.message.empty() &&
             document.visual_state() == layerc::platform::ModuleSidecarState::recovered_backup,
         "unchanged visual candidates retain recovery metadata");
  const auto accepted = document.edit_visual([](auto &candidate)
      -> std::optional<std::string> { candidate.reset(); return std::nullopt; },
      layerc::editor::DocumentChangeKind::immediate);
  expect(accepted.accepted() && accepted.message.empty() && !document.visual() &&
             document.visual_state() == layerc::platform::ModuleSidecarState::absent &&
             document.can_undo() && document.dirty(),
         "an accepted visual removal is one undo step and dirty");
  auto unreadable_visual = fixture_load(document.profile(), std::nullopt);
  unreadable_visual.modules.get<layerc::core::VisualModule>() =
      layerc::platform::VisualSidecarLoad::invalid(
          {.path = "$", .message = "unreadable"}, {});
  document.open({}, std::move(unreadable_visual));
  // Opening resets history, so the real accepted BASE rename comes after it.
  expect(document.edit_profile([](auto &candidate) {
           candidate.name += " history";
         }, DocumentChangeKind::immediate).accepted(),
         "a recorded base rename establishes nonzero visual history");
  auto visual_depth_probe = document;
  int visual_undo_depth{};
  while (visual_depth_probe.undo()) ++visual_undo_depth;
  bool called{};
  const auto blocked = document.edit_visual([&](auto &)
      -> std::optional<std::string> { called = true; return std::nullopt; },
      DocumentChangeKind::immediate);
  auto after_visual_blocked = document;
  int after_visual_depth{};
  while (after_visual_blocked.undo()) ++after_visual_depth;
  expect(blocked.status == ModuleEditStatus::blocked && !called &&
             blocked.message ==
                 "The unreadable visual module is preserved; recalibrate to repair it." &&
             !document.visual() && visual_undo_depth > 0 &&
             after_visual_depth == visual_undo_depth,
         "unreadable visual edits are blocked before callback execution and add no step");
}

void profile_candidate_contracts() {
  using layerc::editor::ModuleEditStatus;
  auto document = fixture();
  const auto before = document.profile();
  const auto invalid = document.edit_profile([](auto &candidate) {
    candidate.cooldowns.front().duration = std::chrono::milliseconds(0);
  }, DocumentChangeKind::immediate);
  expect(invalid.status == ModuleEditStatus::invalid &&
             invalid.message.find("durationMs") != std::string::npos &&
             document.profile() == before && !document.history().can_undo(),
         "invalid base edits report the profile error and preserve values/history");
  const auto change_id = [](auto &candidate) { candidate.id = "changed-id"; };
  const auto visual = document.edit_profile(change_id, DocumentChangeKind::immediate);
  expect(visual.status == ModuleEditStatus::invalid &&
             visual.message == "The edit would invalidate the existing visual module." &&
             document.profile() == before && !document.history().can_undo(),
         "base edits cannot invalidate the visual module");
  // Initial re-seeding with the intended absent visual module.
  document.start_draft(document.profile(), DraftOrigin::fresh, std::nullopt,
                       document.automation());
  add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                      .target = "ability"});
  const auto automation =
      document.edit_profile(change_id, DocumentChangeKind::immediate);
  expect(automation.status == ModuleEditStatus::invalid &&
             automation.message == "The edit would invalidate the existing automation module." &&
             document.profile() == before && !document.history().can_undo(),
         "base edits cannot invalidate the automation module");
  // Initial re-seeding with the intended absent automation module.
  document.start_draft(document.profile(), DraftOrigin::fresh, document.visual(),
                       std::nullopt);
  const CompanionProfile companion{.profile_id = document.profile().id,
                                    .slots = {{.slot_index = 0, .panel_id = "main"}}};
  const auto companion_edit =
      document.edit_profile(change_id, DocumentChangeKind::immediate, &companion);
  expect(companion_edit.status == ModuleEditStatus::invalid &&
             companion_edit.message ==
                 "The edit would invalidate the read-only Game Bar companion module." &&
             document.profile() == before && !document.history().can_undo(),
         "base edits cannot invalidate the read-only companion module");
  const auto unchanged =
      document.edit_profile([](auto &) {}, DocumentChangeKind::immediate);
  expect(unchanged.status == ModuleEditStatus::unchanged && unchanged.message.empty() &&
             !document.history().can_undo(),
         "unchanged base edits leave history alone");
  const auto accepted = document.edit_profile([](auto &candidate) {
    candidate.name = "Accepted base edit";
  }, layerc::editor::DocumentChangeKind::immediate);
  expect(accepted.accepted() && accepted.message.empty() &&
             document.profile().name == "Accepted base edit" &&
             document.can_undo() && document.dirty(),
         "an accepted base edit is one undo step and dirty");
}

} // namespace

namespace {

using namespace layerc::core;
using layerc::test::expect;

GameProfile history_profile(const std::size_t cooldowns) {
  auto profile = layerc::editor::make_new_profile();
  profile.cooldowns.clear();
  for (std::size_t index = 0; index < cooldowns; ++index)
    profile.cooldowns.push_back({.id = "cooldown-" + std::to_string(index),
                                 .name = "Cooldown " + std::to_string(index)});
  return profile;
}

void history_sharing_contracts() {
  using layerc::editor::EditorProfileHistory;
  using layerc::editor::EditorProfileSnapshot;
  // Renames differ in one field only; every heavy member is shared.
  auto profile = history_profile(200);
  const auto original = profile;
  EditorProfileHistory history;
  history.reset(profile, std::nullopt, std::nullopt);
  std::vector<std::string> names;
  for (int index = 0; index < 60; ++index) {
    profile.name = "Rename " + std::to_string(index);
    if (!expect(history.record(profile, std::nullopt, std::nullopt, false),
                "a rename records history"))
      return;
    names.push_back(profile.name);
  }
  expect(history.can_undo(), "rename history can undo");
  // modules, panels, cooldowns, cooldown_groups, hid_bindings, bindings,
  // extra_fields, game.extra_fields, visual_profile and automation_profile: the
  // ten heavy members are unchanged by a rename, so every entry refers to the
  // same copies.
  expect(history.stored_member_copies() == 10,
         "rename-only history shares all ten heavy members");
  expect(history.stored_head_bytes() == 0,
         "rename-only heads retain no member storage");
  for (int index = 0; index < 50; ++index) {
    const auto restored = history.undo(profile, std::nullopt, std::nullopt);
    if (!expect(restored.has_value(), "undo within the retained capacity"))
      return;
    profile = restored->profile;
    expect(profile.name == names[58 - index],
           "undo returns rename history in exact reverse order");
    expect(profile.cooldowns == original.cooldowns,
           "undo restores the original cooldowns");
  }
  expect(!history.undo(profile, std::nullopt, std::nullopt).has_value(),
         "capacity 50 bounds undo");
  for (int index = 0; index < 50; ++index) {
    const auto restored = history.redo(profile, std::nullopt, std::nullopt);
    if (!expect(restored.has_value(), "redo within the retained capacity"))
      return;
    profile = restored->profile;
    expect(profile.name == names[10 + index],
           "redo returns rename history in forward order");
  }
  // record() stores the previous baseline, so a changed member gains a copy
  // when the changed snapshot itself enters history (the undo push).
  auto change_base = history_profile(200);
  EditorProfileHistory changed_history;
  changed_history.reset(change_base, std::nullopt, std::nullopt);
  change_base.name = "Before cooldown change";
  if (!expect(changed_history.record(change_base, std::nullopt, std::nullopt, false),
              "the pre-change rename is recorded"))
    return;
  const auto before_copies = changed_history.stored_member_copies();
  expect(before_copies == 10, "the first entry keeps ten shared members");
  auto edited = change_base;
  edited.cooldowns.front().name = "Changed cooldown";
  if (!expect(changed_history.record(edited, std::nullopt, std::nullopt, false),
              "a cooldown change is recorded"))
    return;
  const auto undone = changed_history.undo(edited, std::nullopt, std::nullopt);
  if (!expect(undone.has_value(), "the cooldown change undoes"))
    return;
  expect(changed_history.stored_member_copies() == before_copies + 1,
         "one changed cooldown adds exactly one cooldown copy");
  expect(*undone == EditorProfileSnapshot{change_base, std::nullopt, std::nullopt},
         "undo materialises the exact pre-change snapshot");
  expect(changed_history.stored_head_bytes() == 0,
         "undo heads retain no member storage");
  const auto redone = changed_history.redo(change_base, std::nullopt, std::nullopt);
  if (!expect(redone.has_value(), "the cooldown change redoes"))
    return;
  expect(*redone == EditorProfileSnapshot{edited, std::nullopt, std::nullopt},
         "redo materialises the exact post-change snapshot");
  // The redo push shares every member with the retained undo entry, and popping
  // the redo entry releases the changed cooldown copy, so the count returns to
  // the ten shared members.
  expect(changed_history.stored_member_copies() == before_copies,
         "the redo push adds no further member copies");
  expect(changed_history.stored_head_bytes() == 0,
         "redo heads retain no member storage");
  // Optional modules round-trip exactly, and every retained visual is visited.
  auto visual_first = VisualProfile{};
  visual_first.profile_id = "visual-profile";
  visual_first.sample_rate_hz = 5;
  auto visual_second = visual_first;
  visual_second.sample_rate_hz = 15;
  auto automation_first = AutomationProfile{};
  automation_first.profile_id = "automation-profile";
  auto automation_second = automation_first;
  automation_second.schema_version = 2;
  auto module_profile = history_profile(200);
  EditorProfileHistory optional_history;
  optional_history.reset(module_profile, visual_first, automation_first);
  module_profile.name = "Optional change";
  if (!expect(optional_history.record(module_profile, visual_second,
                                      automation_second, false),
              "an optional module change is recorded"))
    return;
  const auto earlier =
      optional_history.undo(module_profile, visual_second, automation_second);
  if (!expect(earlier.has_value(), "the optional module change undoes"))
    return;
  expect(earlier->visual_profile == std::optional<VisualProfile>{visual_first} &&
             earlier->automation_profile ==
                 std::optional<AutomationProfile>{automation_first},
         "undo returns the earlier visual and automation optionals exactly");
  const auto later =
      optional_history.redo(earlier->profile, visual_first, automation_first);
  if (!expect(later.has_value(), "the optional module change redoes"))
    return;
  expect(later->visual_profile == std::optional<VisualProfile>{visual_second} &&
             later->automation_profile ==
                 std::optional<AutomationProfile>{automation_second},
         "redo returns the changed visual and automation optionals exactly");
  EditorProfileHistory visit_history;
  visit_history.reset(module_profile, visual_first, automation_first);
  auto visiting = module_profile;
  visiting.name = "First visit";
  if (!expect(visit_history.record(visiting, visual_second, automation_second, false),
              "the first visited edit is recorded"))
    return;
  visiting.name = "Second visit";
  if (!expect(visit_history.record(visiting, visual_first, automation_first, false),
              "the second visited edit is recorded"))
    return;
  std::size_t visited = 0;
  bool saw_first = false;
  bool saw_second = false;
  visit_history.for_each_visual_profile(
      [&](const std::optional<VisualProfile> &visual) {
        ++visited;
        saw_first = saw_first || (visual && visual->sample_rate_hz == 5);
        saw_second = saw_second || (visual && visual->sample_rate_hz == 15);
      });
  // baseline + saved + two undo entries, and no redo entries.
  expect(visited == 4,
         "for_each_visual_profile visits baseline, saved and every entry");
  expect(saw_first && saw_second, "every retained visual profile is visited");
  const auto undone_first =
      visit_history.undo(visiting, visual_first, automation_first);
  const auto undone_second = undone_first
                                 ? visit_history.undo(undone_first->profile, visual_second,
                                                      automation_second)
                                 : std::nullopt;
  if (!expect(undone_first.has_value() && undone_second.has_value(),
              "both visited edits undo"))
    return;
  visited = 0;
  saw_first = false;
  saw_second = false;
  visit_history.for_each_visual_profile(
      [&](const std::optional<VisualProfile> &visual) {
        ++visited;
        saw_first = saw_first || (visual && visual->sample_rate_hz == 5);
        saw_second = saw_second || (visual && visual->sample_rate_hz == 15);
      });
  // baseline + saved + two redo entries, and no undo entries.
  expect(visited == 4, "the visited set covers redo entries as well");
  expect(saw_first && saw_second, "redo entries retain their visual profiles");
  // Game-level opaque fields are shared too: a large game.extra_fields object
  // is retained once, not once per rename entry.
  auto opaque = history_profile(200);
  for (int index = 0; index < 500; ++index)
    opaque.game.extra_fields.emplace("key-" + std::to_string(index),
                                    layerc::core::json::Value(std::string("value")));
  EditorProfileHistory opaque_history;
  opaque_history.reset(opaque, std::nullopt, std::nullopt);
  for (int index = 0; index < 3; ++index) {
    opaque.name = "Opaque rename " + std::to_string(index);
    if (!expect(opaque_history.record(opaque, std::nullopt, std::nullopt, false),
                "an opaque-field rename is recorded"))
      return;
  }
  expect(opaque_history.stored_member_copies() == 10,
         "game-level opaque fields are shared by every rename entry");
}

} // namespace

void editor_cooldown_removal_contracts() {
  group_removal_cases();
  automation_target_cases();
  automation_source_cases();
  accepted_removal_case();
  invalid_bundle_cases();
  commit_bundle_cases();
  automation_candidate_contracts();
  automation_repair_contracts();
  visual_candidate_contracts();
  profile_candidate_contracts();
  history_sharing_contracts();
}

// The shared diagnostic text helpers: sentence joins and dialog words for
// cooldown-group validation errors.
void diagnostic_text_contracts() {
  namespace core = layerc::core;
  using layerc::editor::cooldown_group_error_text;
  using layerc::editor::join_sentences;
  expect(join_sentences("", "Second") == "Second" &&
             join_sentences("First.", "Second") == "First. Second" &&
             join_sentences("First", "Second") == "First. Second" &&
             join_sentences("Really!", "Second") == "Really! Second" &&
             join_sentences("Why?", "") == "Why?",
         "sentences are joined with one full stop and one space");
  std::vector<core::CooldownGroup> groups(2);
  groups[0].id = "pool";
  groups[0].name = "Pool";
  groups[1].id = "defensive";
  groups[1].name = "Defensive";
  expect(cooldown_group_error_text({"$.cooldownGroups[1].id", "duplicate or empty group id"},
                                   groups) ==
             "Group \"Defensive\" (defensive) ID: duplicate or empty group id",
         "a duplicate id names the group and the field by its dialog label");
  groups[1].name.clear();
  expect(cooldown_group_error_text({"$.cooldownGroups[1].name", "is empty"}, groups) ==
             "Group (defensive) Name: is empty",
         "a named field error uses the group id when the name is empty");
  std::vector<core::CooldownGroup> unnamed(1);
  expect(cooldown_group_error_text({"$.cooldownGroups[0].cooldowns", "group requires a member"},
                                   unnamed) ==
             "Group (row 1) Members: group requires a member",
         "an unnamed group without an id is named by its row");
  unnamed[0].cooldown_ids = {"a", "ghost"};
  expect(cooldown_group_error_text({"$.cooldownGroups[0].cooldowns[1]", "unknown cooldown id"},
                                   unnamed) ==
             "Group (row 1) Members \"ghost\": unknown cooldown id",
         "a member path names the member id");
  expect(cooldown_group_error_text({"$.cooldownGroups[0].durationMs", "must be positive"},
                                   unnamed) ==
             "Group (row 1) Duration: must be positive",
         "a known field key uses its dialog label");
  expect(cooldown_group_error_text({"$.cooldownGroups[0].mysteryKey", "is wrong"}, unnamed) ==
             "Group (row 1) mysteryKey: is wrong",
         "an unknown field key stays the raw key");
  expect(cooldown_group_error_text({"$.cooldownGroups", "at most 64 groups are allowed"},
                                   unnamed) == "at most 64 groups are allowed",
         "a path that does not address one group yields the message alone");
  expect(cooldown_group_error_text({"$.name", "is empty"}, unnamed) == "is empty",
         "a non-group path yields the message without the JSON path");
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  core::CooldownGroup duplicate;
  duplicate.id = "shared";
  duplicate.name = "Shared";
  duplicate.cooldown_ids = {profile.cooldowns.front().id};
  profile.cooldown_groups = {duplicate, duplicate};
  const auto error = core::validate_profile(profile);
  const auto text = error ? cooldown_group_error_text(*error, profile.cooldown_groups)
                          : std::string{};
  expect(error && text.starts_with("Group \"") &&
             text.find(" ID: duplicate or empty group id") != std::string::npos &&
             text.find('$') == std::string::npos,
         "a real validator refusal reads as dialog words: " + text);
}
