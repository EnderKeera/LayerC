#include "layerc/core/companion.hpp"
#include "layerc/platform/profile_store.hpp"
#include "test_support.hpp"
#include "../src/gamebar/GameBarBridge.hpp"

#include <filesystem>
#include <fstream>
#include <limits>

using namespace layerc::core;
using namespace layerc::platform;
using namespace layerc::gamebar;

static void test_companion_json_roundtrip() {
  GameProfile base_profile;
  base_profile.id = "test_game";
  Panel p1;
  p1.id = "panel_1";
  Panel p2;
  p2.id = "panel_2";
  base_profile.panels = {p1, p2};

  CompanionProfile profile;
  profile.profile_id = "test_game";
  profile.slots = {
      CompanionSlotMapping{
          .slot_index = 0,
          .panel_id = "panel_1",
          .pinned = true,
          .click_through = false,
          .opacity = 0.85
      },
      CompanionSlotMapping{
          .slot_index = 1,
          .panel_id = "panel_2",
          .pinned = false,
          .click_through = true,
          .opacity = 1.0
      }
  };

  const std::string json = serialize_companion_profile_json(profile);
  auto parsed = parse_companion_profile_json(json, base_profile);
  layerc::test::expect(std::holds_alternative<CompanionProfile>(parsed),
                       "companion json must parse successfully");
  const auto &res = std::get<CompanionProfile>(parsed);
  layerc::test::expect(res.profile_id == "test_game", "profile_id matches");
  layerc::test::expect(res.slots.size() == 2, "2 slots parsed");
  layerc::test::expect(res.slots[0].panel_id == "panel_1", "slot 0 panel");
  layerc::test::expect(res.slots[0].pinned == true, "slot 0 pinned");
  layerc::test::expect(res.slots[0].click_through == false, "slot 0 clickThrough");
  layerc::test::expect(res.slots[0].opacity == 0.85, "slot 0 opacity");
}

static void test_companion_snapshot_bridge() {
  GameProfile base_profile;
  base_profile.id = "hero_game";
  Panel p1;
  p1.id = "panel_1";
  p1.cooldown_ids = {"cd_dash", "cd_heal"};
  base_profile.panels = {p1};

  CooldownDefinition d1;
  d1.id = "cd_dash";
  d1.name = "Dash";
  d1.duration = std::chrono::milliseconds(5000);
  d1.max_charges = 2;

  CooldownDefinition d2;
  d2.id = "cd_heal";
  d2.name = "Heal";
  d2.duration = std::chrono::milliseconds(10000);
  d2.max_charges = 1;

  std::vector<CooldownDefinition> defs = {d1, d2};

  const auto now = MonotonicClock::now();
  CooldownState s1;
  s1.id = "cd_dash";
  s1.available_charges = 1;
  s1.next_charge_at = now + std::chrono::milliseconds(2500);

  CooldownState s2;
  s2.id = "cd_heal";
  s2.available_charges = 1;
  s2.next_charge_at = std::nullopt;

  std::vector<CooldownState> states = {s1, s2};

  auto snapshot = GameBarBridge::create_session_snapshot(
      "hero_game", "session_123", base_profile.panels, defs, states, std::nullopt, now);

  layerc::test::expect(snapshot.profile_id == "hero_game", "snapshot profile_id");
  layerc::test::expect(snapshot.panels.size() == 1, "1 panel snapshot");
  layerc::test::expect(snapshot.panels[0].items.size() == 2, "2 items in panel");
  layerc::test::expect(snapshot.panels[0].items[0].cooldown_id == "cd_dash", "item 0 id");
  layerc::test::expect(snapshot.panels[0].items[0].available_charges == 1, "dash available charges");
  layerc::test::expect(snapshot.panels[0].items[0].remaining_ms == 2500, "dash remaining ms");
  layerc::test::expect(snapshot.panels[0].items[1].is_ready == true, "heal is ready");

  // Roundtrip snapshot json
  const std::string snap_json = serialize_companion_snapshot_json(snapshot);
  auto parsed_snap = parse_companion_snapshot_json(snap_json);
  layerc::test::expect(std::holds_alternative<CompanionSessionSnapshot>(parsed_snap),
                       "snapshot parses from json");
  const auto &unpacked = std::get<CompanionSessionSnapshot>(parsed_snap);
  layerc::test::expect(unpacked.profile_id == "hero_game", "unpacked profile_id");
  layerc::test::expect(unpacked.panels.size() == 1, "unpacked panels");
  layerc::test::expect(unpacked.panels[0].items.size() == 2, "unpacked items");

  // Test bridge stale cutoff
  GameBarBridge bridge;
  bridge.update_snapshot(snapshot);
  layerc::test::expect(bridge.latest_snapshot(now).has_value(), "snapshot valid at now");
  layerc::test::expect(!bridge.latest_snapshot(now + std::chrono::milliseconds(2500)).has_value(),
                       "snapshot stale after 2.5s");
}

static void test_companion_snapshot_rejects_malformed_input() {
  const std::string valid =
      R"({"profileId":"p","sessionKey":"s","timestampMs":1,"panels":[]})";
  const auto expect_rejected = [](const std::string &source,
                                  const char *message) {
    layerc::test::expect(std::holds_alternative<CompanionError>(
                             parse_companion_snapshot_json(source)),
                         message);
  };
  expect_rejected(R"({"profileId":"p","sessionKey":"s"})",
                  "snapshot requires panels");
  expect_rejected(R"({"profileId":"p","sessionKey":"s","timestampMs":0.5,"panels":[]})",
                  "snapshot rejects fractional timestamp");
  expect_rejected(R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[null]})",
                  "snapshot rejects malformed panel");
  expect_rejected(R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":0,"visible":true,"items":[{"cooldownId":"c","name":"C","icon":"","progress":1.1,"availableCharges":0,"maxCharges":1,"remainingMs":0,"isReady":true,"color":""}]}]})",
                  "snapshot rejects out-of-range progress");
  expect_rejected(R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":0,"visible":true,"items":[{"cooldownId":"c","name":"C","icon":"","progress":0,"availableCharges":0.5,"maxCharges":1,"remainingMs":0,"isReady":true,"color":""}]}]})",
                  "snapshot rejects fractional charges");
  expect_rejected(R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":8,"visible":true,"items":[]}]})",
                  "snapshot rejects out-of-range slot");
  expect_rejected(std::string(max_companion_snapshot_bytes + 1, 'x'),
                  "snapshot rejects oversized message");
  (void)valid;
}

static void test_companion_sidecar_survives_unrelated_bundle_save() {
  const auto root = std::filesystem::temp_directory_path() /
                    "LayerC-companion-preservation";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  const auto base_path = root / "profile.json";

  GameProfile base;
  base.id = "preserve";
  base.name = "Preserve";
  base.panels.push_back(Panel{.id = "panel"});
  CompanionProfile companion;
  companion.profile_id = base.id;
  companion.slots = {{.slot_index = 0, .panel_id = "panel"}};
  layerc::test::expect(write_profile_bundle(base_path, base, std::nullopt,
                                            std::nullopt, companion),
                       "companion bundle should be written");

  auto changed = base;
  changed.name = "Changed";
  layerc::test::expect(write_profile_bundle(base_path, changed, std::nullopt),
                       "unrelated bundle save should succeed");
  const auto loaded = load_companion_sidecar(base_path, changed);
  layerc::test::expect(loaded.profile.has_value() &&
                           loaded.profile->profile_id == base.id &&
                           loaded.profile->slots.size() == 1,
                       "unrelated bundle save must preserve companion sidecar");
  std::filesystem::remove_all(root, error);
}

static void test_companion_profile_write_validation() {
  GameProfile base;
  base.id = "p";
  base.panels.push_back(Panel{.id = "panel"});
  CompanionProfile duplicate_panel;
  duplicate_panel.profile_id = "p";
  duplicate_panel.slots = {{.slot_index = 0, .panel_id = "panel"},
                           {.slot_index = 1, .panel_id = "panel"}};
  const auto error = validate_companion_profile(duplicate_panel, base);
  layerc::test::expect(!error.message.empty(),
                       "validator rejects duplicate panel assignments");
  CompanionProfile invalid_opacity;
  invalid_opacity.profile_id = "p";
  invalid_opacity.slots = {{.slot_index = 0, .panel_id = "panel", .opacity = 2.0}};
  layerc::test::expect(!validate_companion_profile(invalid_opacity, base).message.empty(),
                       "validator rejects out-of-range opacity");
}

int main() {
  test_companion_json_roundtrip();
  test_companion_snapshot_bridge();
  test_companion_snapshot_rejects_malformed_input();
  test_companion_profile_write_validation();
  test_companion_sidecar_survives_unrelated_bundle_save();
  return 0;
}
