#include "editor_util.hpp"
#include "profile_document.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace {
using namespace layerc::core;
using layerc::editor::ProfileDocument;
using layerc::test::expect;

ProfileDocument fixture() {
  ProfileDocument document;
  document.profile() = layerc::editor::make_new_profile();
  document.profile().game.executable_name = "fixture.exe";
  auto other = document.profile().cooldowns.front();
  other.id = "other";
  other.name = "Other";
  document.profile().cooldowns.push_back(std::move(other));
  document.profile().panels.front().cooldown_ids.push_back("other");
  document.profile().bindings.push_back(
      {.cooldown_id = "other",
       .chord = InputChord{{{InputDevice::keyboard, 0x13, 0}}}});
  document.visual().emplace();
  document.visual()->profile_id = document.profile().id;
  document.visual()->observations.push_back(
      layerc::core::PixelRule{.id = "ready", .name = "Ready"});
  document.visual()->triggers.push_back(
      {.cooldown_id = "ability", .observation_id = "ready"});
  document.mark_visual_edited();
  document.history().reset(document.profile(), document.visual(),
                           document.automation());
  return document;
}

void add_rule(ProfileDocument &document, AutomationAction action,
              const bool enabled = true) {
  document.automation().emplace();
  document.automation()->profile_id = document.profile().id;
  AutomationRule rule;
  rule.id = "fixture-rule";
  rule.event = AutomationEventKind::session_started;
  rule.actions.push_back(std::move(action));
  document.automation()->automations.push_back(
      {.id = "fixture-automation", .name = "Fixture", .enabled = enabled,
       .rules = {std::move(rule)}});
  document.mark_automation_edited();
  document.history().reset(document.profile(), document.visual(),
                           document.automation());
}

void expect_valid(const ProfileDocument &document) {
  const auto parsed =
      parse_profile_json(serialize_profile_json(document.profile()));
  const auto *base_error = std::get_if<ProfileError>(&parsed);
  expect(base_error == nullptr,
         base_error ? base_error->path + ": " + base_error->message
                    : "the complete base profile must remain parser-valid");
  if (document.visual()) {
    const auto error =
        validate_visual_profile(*document.visual(), document.profile());
    expect(error.message.empty(), error.path + ": " + error.message);
  }
  if (document.automation()) {
    const auto error =
        validate_automation_profile(*document.automation(), document.profile());
    expect(error.message.empty(), error.path + ": " + error.message);
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
    document.profile().cooldown_groups.push_back(
        {.id = "fixture-group", .name = "Fixture group", .mode = mode,
         .cooldown_ids = {"ability", "other"},
         .duration = std::chrono::seconds(5)});
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
    auto &rule = document.automation()->automations.front().rules.front();
    rule.event = AutomationEventKind::cooldown_ready;
    rule.source_filter = "ability";
    expect_valid(document);
    expect_rejected(document, "cooldown event source 'ability'");
  }

  {
    auto document = fixture();
    add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                        .target = "other"});
    auto &rule = document.automation()->automations.front().rules.front();
    rule.event = AutomationEventKind::visual_observation;
    rule.source_filter = "ready";
    expect_valid(document);
    expect_rejected(document, "visual observation 'ready'");
  }

  {
    auto document = fixture();
    document.profile().panels.front().id = "ability-bar";
    document.profile().panels.front().cooldown_ids = {"ability"};
    auto other = document.profile().panels.front();
    other.id = "other-bar";
    other.cooldown_ids = {"other"};
    document.profile().panels.push_back(std::move(other));
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
    document.automation()->variables.push_back(
        {.id = "ability", .type = AutomationValueType::integer,
         .default_value = AutomationValue(std::int64_t{0})});
    auto &rule = document.automation()->automations.front().rules.front();
    rule.event = AutomationEventKind::timer_fired;
    rule.source_filter = "ability";
    document.history().reset(document.profile(), document.visual(),
                             document.automation());
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
    expect(document.history().record(document.profile(), document.visual(),
                                     document.automation()),
           "accepted deletion must record exactly one complete history transaction");
    const auto undo = document.history().undo(
        document.profile(), document.visual(), document.automation());
    expect(undo && *undo == before && !document.history().can_undo(),
           "one undo must restore the complete original bundle");
    if (undo) {
      document.profile() = undo->profile;
      document.visual() = undo->visual_profile;
      document.automation() = undo->automation_profile;
      expect_valid(document);
      const auto redo = document.history().redo(
          document.profile(), document.visual(), document.automation());
      expect(redo && redo->profile.cooldowns.size() == 1 &&
                 redo->visual_profile->triggers.empty(),
             "one redo must restore the complete accepted removal");
      if (redo) {
        document.profile() = redo->profile;
        document.visual() = redo->visual_profile;
        document.automation() = redo->automation_profile;
        expect_valid(document);
      }
    }
  }
}

void invalid_bundle_cases() {
  {
    auto document = fixture();
    document.profile().cooldowns.back().duration = std::chrono::milliseconds(0);
    expect_rejected(document, "durationMs");
  }

  {
    auto document = fixture();
    add_rule(document, {.kind = AutomationActionKind::trigger_cooldown,
                        .target = "ability"});
    document.profile().name += " edited";
    (void)document.history().record(document.profile(), document.visual(),
                                    document.automation());
    document.set_dirty(true);
    expect_rejected(document, "fixture-rule");
  }

  {
    auto document = fixture();
    document.set_automation(layerc::platform::AutomationSidecarLoad::invalid(
        {.path = "$", .message = "fixture invalid sidecar"}, {}));
    expect_rejected(document, "sidecar");
  }
}

} // namespace

void editor_cooldown_removal_contracts() {
  group_removal_cases();
  automation_target_cases();
  automation_source_cases();
  accepted_removal_case();
  invalid_bundle_cases();
}
