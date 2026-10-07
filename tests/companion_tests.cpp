#include "../src/daemon/companion_session_selection.hpp"
#include "layerc/core/companion_snapshot.hpp"
#include "../src/gamebar/GameBarTransport.hpp"
#include "../src/gamebar/GameBarTransportState.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/window_query.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <limits>

using namespace layerc::core;
using namespace layerc::platform;
using namespace layerc::gamebar;

static const std::string *serialized_json(
    const CompanionSerializeResult &result) {
  return std::get_if<std::string>(&result);
}

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
  profile.slots = {CompanionSlotMapping{.slot_index = 0,
                                        .panel_id = "panel_1",
                                        .pinned = true,
                                        .click_through = false,
                                        .opacity = 0.85},
                   CompanionSlotMapping{.slot_index = 1,
                                        .panel_id = "panel_2",
                                        .pinned = false,
                                        .click_through = true,
                                        .opacity = 1.0}};

  const std::string json = serialize_companion_profile_json(profile);
  auto parsed = parse_companion_profile_json(json, base_profile);
  layerc::test::expect(std::holds_alternative<CompanionProfile>(parsed),
                       "companion json must parse successfully");
  const auto &res = std::get<CompanionProfile>(parsed);
  layerc::test::expect(res.profile_id == "test_game", "profile_id matches");
  layerc::test::expect(res.slots.size() == 2, "2 slots parsed");
  layerc::test::expect(res.slots[0].panel_id == "panel_1", "slot 0 panel");
  layerc::test::expect(res.slots[0].pinned == true, "slot 0 pinned");
  layerc::test::expect(res.slots[0].click_through == false,
                       "slot 0 clickThrough");
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
  s1.max_charges = 2;
  s1.next_charge_at = now + std::chrono::milliseconds(2500);
  s1.recharge_duration = d1.duration;

  CooldownState s2;
  s2.id = "cd_heal";
  s2.available_charges = 1;
  s2.max_charges = 1;
  s2.next_charge_at = std::nullopt;
  s2.recharge_duration = d2.duration;

  std::vector<CooldownState> states = {s1, s2};

  auto json = build_companion_snapshot(
      "hero_game", "session_123", base_profile.panels, defs, states,
      std::nullopt, now);
  layerc::test::expect(!json.empty(), "valid session produces bounded JSON");
  auto snapshot = std::get<CompanionSessionSnapshot>(
      parse_companion_snapshot_json(json));

  layerc::test::expect(snapshot.profile_id == "hero_game",
                       "snapshot profile_id");
  layerc::test::expect(snapshot.panels.size() == 1, "1 panel snapshot");
  layerc::test::expect(snapshot.panels[0].items.size() == 2,
                       "2 items in panel");
  layerc::test::expect(snapshot.available_panels.size() == 1 &&
                           snapshot.available_panels[0].panel_id == "panel_1",
                       "snapshot carries the bounded settings panel catalog");
  layerc::test::expect(
      snapshot.preferences.size() == 1 &&
          snapshot.preferences[0].slot_index == 0 &&
          snapshot.preferences[0].panel_id == "panel_1",
      "snapshot carries default slot preferences without a sidecar");
  layerc::test::expect(snapshot.panels[0].items[0].cooldown_id == "cd_dash",
                       "item 0 id");
  layerc::test::expect(snapshot.panels[0].items[0].available_charges == 1,
                       "dash available charges");
  layerc::test::expect(snapshot.panels[0].items[0].remaining_ms == 2500,
                       "dash remaining ms");
  layerc::test::expect(snapshot.panels[0].items[1].is_ready == true,
                       "heal is ready");
  // layer21-F8: progress convention is 'elapsed toward ready' (1.0 = ready).
  layerc::test::expect(snapshot.panels[0].items[0].progress < 1.0 &&
                           snapshot.panels[0].items[0].progress > 0.0,
                       "recharging item progress is between 0 and 1");
  layerc::test::expect(snapshot.panels[0].items[1].progress == 1.0,
                       "ready item progress is 1.0");

  // Roundtrip snapshot json
  const auto serialized = serialize_companion_snapshot_json(snapshot);
  layerc::test::expect(serialized_json(serialized) != nullptr,
                       "valid snapshot serializes");
  auto parsed_snap = parse_companion_snapshot_json(*serialized_json(serialized));
  layerc::test::expect(
      std::holds_alternative<CompanionSessionSnapshot>(parsed_snap),
      "snapshot parses from json");
  const auto &unpacked = std::get<CompanionSessionSnapshot>(parsed_snap);
  layerc::test::expect(unpacked.profile_id == "hero_game",
                       "unpacked profile_id");
  layerc::test::expect(unpacked.panels.size() == 1, "unpacked panels");
  layerc::test::expect(unpacked.panels[0].items.size() == 2, "unpacked items");
  layerc::test::expect(unpacked == snapshot,
                       "snapshot settings metadata round trips exactly");

  snapshot.preferences.front().opacity = 0.0;
  const auto transparent = serialize_companion_snapshot_json(snapshot);
  layerc::test::expect(serialized_json(transparent) != nullptr,
                       "zero-opacity snapshot preference serializes");
  const auto transparent_parsed =
      parse_companion_snapshot_json(*serialized_json(transparent));
  layerc::test::expect(
      std::holds_alternative<CompanionSessionSnapshot>(transparent_parsed) &&
          std::get<CompanionSessionSnapshot>(transparent_parsed) == snapshot,
      "zero-opacity snapshot preference round trips exactly");

}

static void test_companion_shared_pool_runtime_view() {
  using namespace std::chrono_literals;
  const std::vector<CooldownDefinition> definitions{
      {.id = "a", .name = "A", .duration = 1s},
      {.id = "b", .name = "B", .duration = 10s}};
  const std::vector<Panel> panels{{.id = "panel", .cooldown_ids = {"a", "b"}}};
  const TimePoint start{};
  for (const auto recovery : {RecoveryMode::single, RecoveryMode::sequential,
                              RecoveryMode::parallel}) {
    const std::vector<CooldownGroup> groups{
        {.id = "pool", .name = "Pool", .mode = CooldownGroupMode::shared_pool,
         .cooldown_ids = {"a", "b"}, .duration = 5s, .max_charges = 3,
         .recovery = recovery}};
    CooldownEngine engine(definitions, groups);
    layerc::test::expect(engine.trigger("a", start),
                         "pool presentation fixture consumes a charge");
    const auto states = engine.snapshot(start + 1s);
    const auto json = build_companion_snapshot(
        "profile", "session", panels, definitions, states, std::nullopt, start + 1s);
    const auto parsed = parse_companion_snapshot_json(json);
    const auto *snapshot = std::get_if<CompanionSessionSnapshot>(&parsed);
    layerc::test::expect(snapshot != nullptr && snapshot->panels.size() == 1 &&
                             snapshot->panels.front().items.size() == 2,
                         "CM-03: valid pool charges do not clear companion items");
    if (snapshot == nullptr || snapshot->panels.empty() ||
        snapshot->panels.front().items.size() != 2)
      continue;
    for (std::size_t index = 0; index < definitions.size(); ++index) {
      const auto &item = snapshot->panels.front().items[index];
      const auto native_progress =
          cooldown_progress(states[index], start + 1s);
      layerc::test::expect(
          item.cooldown_id == definitions[index].id &&
              item.name == definitions[index].name && item.max_charges == 3 &&
              item.available_charges == 2 && item.remaining_ms == 4000 &&
              !item.is_ready && item.progress > 0.199 && item.progress < 0.201 &&
              native_progress > 0.799 && native_progress < 0.801 &&
              cooldown_status_text(definitions[index], states[index], start + 1s) ==
                  definitions[index].name + "  4.0s  [2]",
          "CM-03: all presentation uses the owner duration and charges, with public labels");
    }
    const auto ready_states = engine.snapshot(start + 5s);
    const auto ready_json = build_companion_snapshot(
        "profile", "session", panels, definitions, ready_states, std::nullopt, start + 5s);
    const auto ready_parsed = parse_companion_snapshot_json(ready_json);
    const auto *ready = std::get_if<CompanionSessionSnapshot>(&ready_parsed);
    layerc::test::expect(
        ready != nullptr && ready->panels.front().items.size() == 2 &&
            ready->panels.front().items.front().max_charges == 3 &&
            ready->panels.front().items.front().available_charges == 3 &&
            ready->panels.front().items.front().is_ready &&
            ready->panels.front().items.front().progress == 1.0 &&
            cooldown_progress(ready_states.front(), start + 5s) == 1.0,
        "CM-03: fully recovered pools retain effective charges and ready progress");
  }
}

static void test_companion_bridge_rejects_invalid_not_oversize() {
  using namespace std::chrono_literals;
  const std::vector<CooldownDefinition> definitions{
      {.id = "a", .name = "A", .duration = 5s, .max_charges = 2}};
  const std::vector<Panel> panels{{.id = "panel", .cooldown_ids = {"a"}}};
  const std::vector<CooldownState> invalid_states{
      {.id = "a", .available_charges = 3, .max_charges = 2}};
  layerc::test::expect(build_companion_snapshot(
                          "profile", "session", panels, definitions, invalid_states,
                          std::nullopt, TimePoint{}).empty(),
                       "CM-03: invalid charges cannot be hidden by oversize shrinking");
  auto invalid_text = definitions;
  invalid_text.front().name.clear();
  layerc::test::expect(build_companion_snapshot(
                          "profile", "session", panels, invalid_text, {},
                          std::nullopt, TimePoint{}).empty(),
                       "CM-03: invalid item text cannot turn into a valid empty panel");
}

static void test_companion_snapshot_bounds_and_session_identity() {
  GameProfile base_profile;
  base_profile.id = "bounded";
  Panel panel;
  panel.id = "many";
  std::vector<CooldownDefinition> definitions;
  for (std::size_t index = 0; index < max_companion_snapshot_items + 8U;
       ++index) {
    const auto id = "cooldown-" + std::to_string(index);
    panel.cooldown_ids.push_back(id);
    definitions.push_back(
        {.id = id, .name = id, .duration = std::chrono::seconds(1)});
  }
  base_profile.panels.push_back(std::move(panel));
  const auto now = MonotonicClock::now();
  const auto json = build_companion_snapshot(
      base_profile.id, window_session_key(reinterpret_cast<HWND>(1)),
      base_profile.panels, definitions, {}, std::nullopt, now);
  const auto snapshot = std::get<CompanionSessionSnapshot>(
      parse_companion_snapshot_json(json));
  layerc::test::expect(
      snapshot.panels.size() == 1 &&
          snapshot.panels.front().items.size() ==
              max_companion_snapshot_items &&
          serialized_json(serialize_companion_snapshot_json(snapshot)) != nullptr,
      "bridge truncates oversized panels to the serializable item bound");

  const auto first = window_session_key(reinterpret_cast<HWND>(100));
  const auto second = window_session_key(reinterpret_cast<HWND>(200));
  layerc::test::expect(
      first != second,
      "matched windows receive distinct companion session keys");
  layerc::test::expect(window_session_key(nullptr).compare("global") == 0,
                       "global companion sessions use a stable discriminator");

  const std::string maximum_profile_id(max_companion_text_bytes, 'p');
  const auto maximum_id_json = build_companion_snapshot(
      maximum_profile_id, window_session_key(reinterpret_cast<HWND>(100)),
      base_profile.panels, definitions, {}, std::nullopt, now);
  layerc::test::expect(
      !maximum_id_json.empty(),
      "session key remains bounded for a maximum-length profile ID");
}

static void test_bridge_truncates_worst_case_snapshot() {
  const auto bounded = [](std::string value, const char fill) {
    value.resize(max_companion_text_bytes, fill);
    return value;
  };
  const auto item_text = [](std::string value, const char fill) {
    value.resize(max_companion_text_bytes / 2U, fill);
    return value;
  };

  GameProfile base_profile;
  base_profile.id = "worst-case";
  base_profile.name = "Worst case";
  base_profile.game.global = true;
  std::vector<CooldownDefinition> definitions;
  for (std::size_t index = 0; index < max_companion_snapshot_items; ++index) {
    definitions.push_back(
        {.id = item_text("cooldown-" + std::to_string(index) + "-", 'c'),
         .name = item_text("Cooldown " + std::to_string(index) + " ", 'n'),
         .duration = std::chrono::seconds(1)});
  }
  base_profile.cooldowns = definitions;
  base_profile.bindings.push_back(
      {.cooldown_id = definitions.front().id,
       .chord = {.tokens = {{.device = InputDevice::keyboard, .code = 1}}}});

  CompanionProfile companion;
  companion.profile_id = base_profile.id;
  for (std::size_t index = 0; index < max_companion_available_panels;
       ++index) {
    Panel panel;
    panel.id = bounded("panel-" + std::to_string(index) + "-", 'p');
    panel.name = bounded("Panel " + std::to_string(index) + " ", 'n');
    if (index < max_companion_slots) {
      for (const auto &definition : definitions)
        panel.cooldown_ids.push_back(definition.id);
      companion.slots.push_back(
          {.slot_index = static_cast<std::uint32_t>(index),
           .panel_id = panel.id});
    } else {
      panel.cooldown_ids.push_back(definitions.front().id);
    }
    base_profile.panels.push_back(std::move(panel));
  }

  const auto profile_json = serialize_profile_json(base_profile);
  layerc::test::expect(
      profile_json.size() <= max_profile_json_bytes &&
          std::holds_alternative<GameProfile>(parse_profile_json(profile_json)) &&
          !validate_companion_profile(companion, base_profile),
      "worst-case bridge input is accepted by the profile boundaries");

  const auto json = build_companion_snapshot(
      base_profile.id, "session", base_profile.panels, definitions, {},
      companion, MonotonicClock::now());
  const auto snapshot = std::get<CompanionSessionSnapshot>(
      parse_companion_snapshot_json(json));
  layerc::test::expect(
      snapshot.available_panels.size() == max_companion_slots &&
          !snapshot.panels.empty() && snapshot.panels.back().items.empty() &&
          serialized_json(serialize_companion_snapshot_json(snapshot)) != nullptr,
      "bridge preserves mapped metadata and truncates live data to 64 KiB");
}

static void test_all_eight_slots_persist_and_publish() {
  GameProfile base_profile;
  base_profile.id = "eight-slots";
  base_profile.name = "Eight slots";
  CompanionProfile profile;
  profile.profile_id = base_profile.id;
  for (std::size_t index = 0; index < max_companion_slots; ++index) {
    const auto panel_id = "panel-" + std::to_string(index + 1U);
    Panel panel;
    panel.id = panel_id;
    panel.name = "Panel " + std::to_string(index + 1U);
    base_profile.panels.push_back(std::move(panel));
    profile.slots.push_back(
        {.slot_index = static_cast<std::uint32_t>(index),
         .panel_id = panel_id,
         .pinned = index % 2U == 0U,
         .click_through = index % 2U != 0U,
         .opacity = 1.0 - static_cast<double>(index) * 0.05});
  }

  const auto now = MonotonicClock::now();
  const auto json = build_companion_snapshot(
      base_profile.id, "global", base_profile.panels, {}, {}, profile, now);
  const auto snapshot = std::get<CompanionSessionSnapshot>(
      parse_companion_snapshot_json(json));
  layerc::test::expect(
      snapshot.panels.size() == max_companion_slots &&
          snapshot.available_panels.size() == max_companion_slots &&
          snapshot.preferences == profile.slots,
      "all eight mapped slots publish in one bounded snapshot");
  for (std::size_t index = 0; index < max_companion_slots; ++index) {
    layerc::test::expect(
        snapshot.panels[index].slot_index == index &&
            snapshot.panels[index].panel_id == profile.slots[index].panel_id,
        "each published panel retains its distinct slot mapping");
  }
  const auto serialized = serialize_companion_snapshot_json(snapshot);
  layerc::test::expect(serialized_json(serialized) != nullptr &&
                           std::holds_alternative<CompanionSessionSnapshot>(
                               parse_companion_snapshot_json(
                                   *serialized_json(serialized))),
                        "the simultaneous eight-slot snapshot round trips");

  const layerc::test::TempDirectory directory("LayerC-companion-eight-slots");
  const auto base_path = directory.path() / "profile.json";
  layerc::test::expect(
      write_profile_bundle(base_path, base_profile, std::nullopt,
                           std::nullopt, profile).ok(),
      "all eight slot preferences save through the profile transaction");
  const auto loaded = load_companion_sidecar(base_path, base_profile);
  layerc::test::expect(loaded.state() == ModuleSidecarState::valid &&
                           loaded.usable() &&
                           *loaded.profile() == profile,
                       "all eight slot preferences survive sidecar reload");
}

static void test_companion_transport_transitions() {
  GameBarTransportState state;
  using Link = GameBarTransportState::Link;
  layerc::test::expect(state.link == Link::stopped && !state.retry.ready &&
                           !state.retry.needed && state.retry.delay_ms == 100,
                       "transport starts inert without resource allocation");
  {
    std::lock_guard lock(state.mutex);
    state.enter(Link::idle);
    state.enter(Link::opening);
    state.enter(Link::connected, L"");
    state.enter(Link::sending);
    state.enter(Link::idle, L"Game Bar snapshot send failed");
    state.retry.needed = true;
    layerc::test::expect(state.last_error == L"Game Bar snapshot send failed" &&
                             state.retry.needed && state.link == Link::idle,
                         "send failure retains diagnostics until retry");
    state.enter(Link::stopped);
  }
  layerc::test::expect(state.link == Link::stopped &&
                           state.last_error == L"Game Bar snapshot send failed",
                       "stopping does not hide the last failure reason");
}

static void test_companion_package_identity() {
  layerc::test::expect(GameBarTransport::is_expected_package_identity(
                           GameBarTransport::package_name,
                           GameBarTransport::package_publisher,
                           GameBarTransport::package_family_name,
                           GameBarTransport::package_major_version,
                           GameBarTransport::package_minor_version),
                       "the declared Game Bar package identity is accepted");
  layerc::test::expect(
      !GameBarTransport::is_expected_package_identity(
          GameBarTransport::package_name, L"CN=Foreign Publisher",
          GameBarTransport::package_family_name,
          GameBarTransport::package_major_version,
          GameBarTransport::package_minor_version),
      "a same-name package from another publisher is rejected");
  layerc::test::expect(!GameBarTransport::is_expected_package_identity(
                           GameBarTransport::package_name,
                           GameBarTransport::package_publisher,
                           GameBarTransport::package_family_name, 3, 0),
                       "an incompatible Game Bar package version is rejected");
  layerc::test::expect(!GameBarTransport::is_expected_package_identity(
                           GameBarTransport::package_name,
                           GameBarTransport::package_publisher,
                           L"LayerC.GameBar.v1_foreignfamily", 2, 4),
                       "a different package family is rejected");
  if (!GameBarTransport::current_process_is_expected_package()) {
    DWORD handles_before{};
    DWORD handles_constructed{};
    DWORD handles_destroyed{};
    (void)GetProcessHandleCount(GetCurrentProcess(), &handles_before);
    {
      GameBarTransport transport;
      (void)GetProcessHandleCount(GetCurrentProcess(), &handles_constructed);
      layerc::test::expect(
          handles_constructed == handles_before,
          "constructing the unpackaged Game Bar transport owns no handle");
      layerc::test::expect(
          !transport.start() && transport.resource_handle_count() == 0,
          "unpackaged daemon does not allocate the Game Bar App Service");
    }
    (void)GetProcessHandleCount(GetCurrentProcess(), &handles_destroyed);
    layerc::test::expect(handles_destroyed == handles_before,
                         "unpackaged Game Bar transport leaks no handle");
  }
}

static void test_companion_duplicate_slot_contract() {
  const std::string panel =
      R"({"panelId":"a","slotIndex":2,"items":[]})";
  const std::string valid =
      R"({"profileId":"p","sessionKey":"s","timestampMs":1,"panels":[)" +
      panel +
      R"(],"availablePanels":[{"panelId":"a","name":"A"}],"preferences":[{"slotIndex":2,"panelId":"a","pinned":false,"clickThrough":true,"opacity":1.0}]})";
  const auto parsed = parse_companion_snapshot_json(valid);
  const auto *baseline = std::get_if<CompanionSessionSnapshot>(&parsed);
  layerc::test::expect(baseline != nullptr,
                       "duplicate-slot fixture starts from accepted JSON");
  if (baseline == nullptr)
    return;
  layerc::test::expect(
      serialized_json(serialize_companion_snapshot_json(*baseline)) != nullptr,
      "duplicate-slot fixture starts from a serializable snapshot");

  // Duplicate the same complete panel, retaining the catalog and preferences.
  // A different panel with the same slot would fail correspondence first.
  auto duplicate_json = valid;
  duplicate_json.replace(duplicate_json.find(panel), panel.size(),
                         panel + "," + panel);
  const auto duplicate_parsed = parse_companion_snapshot_json(duplicate_json);
  const auto *parse_error = std::get_if<CompanionError>(&duplicate_parsed);
  layerc::test::expect(
      parse_error && parse_error->path == "$.panels[1].slotIndex" &&
          parse_error->message == "duplicate slotIndex in snapshot",
      "parser reaches the exact duplicate-panel-slot guard");

  auto duplicate = *baseline;
  duplicate.panels.push_back(baseline->panels.front());
  const auto serialized = serialize_companion_snapshot_json(duplicate);
  const auto *serialize_error = std::get_if<CompanionError>(&serialized);
  layerc::test::expect(
      serialize_error && serialize_error->path == "$.panels[1].slotIndex" &&
          serialize_error->message == "duplicate slotIndex in snapshot",
      "serializer reaches the exact duplicate-panel-slot guard");
}

static void test_companion_declarative_wire() {
  GameProfile base;
  base.id = "p";
  Panel panel;
  panel.id = "a";
  base.panels.push_back(panel);
  const std::string historical_profile =
      R"({"schemaVersion":1,"profileId":"p","slots":[{"slotIndex":2,"panelId":"a","pinned":false,"clickThrough":true,"opacity":1}]})";
  const auto parsed_profile = parse_companion_profile_json(historical_profile, base);
  const auto *profile = std::get_if<CompanionProfile>(&parsed_profile);
  layerc::test::expect(profile != nullptr,
                       "historical companion sidecar order remains accepted");
  if (profile == nullptr)
    return;
  const std::string golden_profile =
      R"({"profileId":"p","schemaVersion":1,"slots":[{"clickThrough":true,"opacity":1,"panelId":"a","pinned":false,"slotIndex":2}]})";
  layerc::test::expect(serialize_companion_profile_json(*profile) == golden_profile,
                       "companion sidecar uses deterministic Value key order");
  const auto reparsed_profile = parse_companion_profile_json(golden_profile, base);
  layerc::test::expect(std::get_if<CompanionProfile>(&reparsed_profile) &&
                           std::get<CompanionProfile>(reparsed_profile) == *profile,
                       "new sidecar bytes preserve historical semantics");
  std::string padded_profile = golden_profile;
  padded_profile.resize(max_companion_sidecar_bytes + 1U, ' ');
  const auto oversized_profile = parse_companion_profile_json(padded_profile, base);
  const auto *profile_error = std::get_if<CompanionError>(&oversized_profile);
  layerc::test::expect(
      profile_error && profile_error->path == "$" &&
          profile_error->message.find("exceeds") != std::string::npos,
      "sidecar rejects oversized bytes through the bounded parser");
  padded_profile.resize(max_companion_sidecar_bytes);
  layerc::test::expect(std::holds_alternative<CompanionProfile>(
                           parse_companion_profile_json(padded_profile, base)),
                       "sidecar at exactly the byte bound parses");

  const std::string historical_snapshot =
      R"({"profileId":"p","sessionKey":"s","timestampMs":9223372036854775807,"panels":[{"panelId":"a","slotIndex":2,"items":[{"cooldownId":"c","name":"C\n\"","progress":0.5,"availableCharges":0,"maxCharges":1,"remainingMs":9007199254740993,"isReady":false}]}],"availablePanels":[{"panelId":"a","name":"A"}],"preferences":[{"slotIndex":2,"panelId":"a","pinned":false,"clickThrough":true,"opacity":1}]})";
  const auto parsed_snapshot = parse_companion_snapshot_json(historical_snapshot);
  const auto *snapshot = std::get_if<CompanionSessionSnapshot>(&parsed_snapshot);
  layerc::test::expect(snapshot != nullptr,
                       "historical snapshot order and exact integers parse");
  if (snapshot == nullptr)
    return;
  const std::string golden_snapshot =
      R"({"availablePanels":[{"name":"A","panelId":"a"}],"panels":[{"items":[{"availableCharges":0,"cooldownId":"c","isReady":false,"maxCharges":1,"name":"C\n\"","progress":0.5,"remainingMs":9007199254740993}],"panelId":"a","slotIndex":2}],"preferences":[{"clickThrough":true,"opacity":1,"panelId":"a","pinned":false,"slotIndex":2}],"profileId":"p","sessionKey":"s","timestampMs":9223372036854775807})";
  const auto serialized = serialize_companion_snapshot_json(*snapshot);
  const auto *wire = serialized_json(serialized);
  layerc::test::expect(wire && *wire == golden_snapshot,
                       "snapshot golden preserves escaping and integers");
  if (wire) {
    const auto reparsed = parse_companion_snapshot_json(*wire);
    layerc::test::expect(std::get_if<CompanionSessionSnapshot>(&reparsed) &&
                             std::get<CompanionSessionSnapshot>(reparsed) == *snapshot,
                         "snapshot emission round-trips without numeric rounding");
  }
  auto invalid = *snapshot;
  invalid.timestamp_ms = std::numeric_limits<std::uint64_t>::max();
  const auto rejected = serialize_companion_snapshot_json(invalid);
  const auto *error = std::get_if<CompanionError>(&rejected);
  layerc::test::expect(error && error->path == "$.timestampMs",
                       "serializer rejects timestamps outside the reader integer range");
}

static void test_companion_snapshot_rejects_malformed_input() {
  const std::string valid =
      R"({"profileId":"p","sessionKey":"s","timestampMs":1,"panels":[],"availablePanels":[],"preferences":[]})";
  const auto expect_rejected = [](const std::string &source,
                                  const char *message) {
    layerc::test::expect(std::holds_alternative<CompanionError>(
                             parse_companion_snapshot_json(source)),
                         message);
  };
  layerc::test::expect(std::holds_alternative<CompanionSessionSnapshot>(
                           parse_companion_snapshot_json(valid)),
                       "complete bounded snapshot parses");
  expect_rejected(R"({"profileId":"p","sessionKey":"s"})",
                  "snapshot requires panels");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0.5,"panels":[]})",
      "snapshot rejects fractional timestamp");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[null]})",
      "snapshot rejects malformed panel");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":0,"items":[{"cooldownId":"c","name":"C","progress":1.1,"availableCharges":0,"maxCharges":1,"remainingMs":0,"isReady":true}]}]})",
      "snapshot rejects out-of-range progress");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":0,"items":[{"cooldownId":"c","name":"C","progress":0,"availableCharges":0.5,"maxCharges":1,"remainingMs":0,"isReady":true}]}]})",
      "snapshot rejects fractional charges");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[{"panelId":"x","slotIndex":8,"items":[]}]})",
      "snapshot rejects out-of-range slot");
  expect_rejected(
      R"({"profileId":"p","sessionKey":"s","timestampMs":0,"panels":[],"availablePanels":[{"panelId":"a","name":"A"}],"preferences":[{"slotIndex":0,"panelId":"a","pinned":false,"clickThrough":true,"opacity":1.0},{"slotIndex":1,"panelId":"a","pinned":false,"clickThrough":true,"opacity":1.0}]})",
      "snapshot rejects one panel assigned to duplicate preference slots");
  // Admission is owned by json::parse_bounded: otherwise valid JSON padded past
  // the bound is rejected at the root before parsing.
  std::string padded_snapshot = valid;
  padded_snapshot.resize(max_companion_snapshot_bytes + 1U, ' ');
  const auto oversized_snapshot = parse_companion_snapshot_json(padded_snapshot);
  const auto *snapshot_error = std::get_if<CompanionError>(&oversized_snapshot);
  layerc::test::expect(
      snapshot_error && snapshot_error->path == "$" &&
          snapshot_error->message.find("exceeds") != std::string::npos,
      "snapshot rejects oversized message through the bounded parser");
  padded_snapshot.resize(max_companion_snapshot_bytes);
  layerc::test::expect(std::holds_alternative<CompanionSessionSnapshot>(
                           parse_companion_snapshot_json(padded_snapshot)),
                       "snapshot at exactly the byte bound parses");

  // Serialization preserves the exact validation or byte-budget failure.
  CompanionSessionSnapshot invalid;
  invalid.profile_id = "";
  const auto invalid_result = serialize_companion_snapshot_json(invalid);
  layerc::test::expect(
      std::holds_alternative<CompanionError>(invalid_result) &&
          std::get<CompanionError>(invalid_result).path == "$.profileId",
      "invalid snapshot serialization returns its validation error");
  CompanionSessionSnapshot oversized;
  oversized.profile_id = "p";
  oversized.session_key = "s";
  for (std::size_t index = 0; index < max_companion_available_panels; ++index) {
    auto id = std::to_string(index) + "-";
    id.resize(max_companion_text_bytes, 'i');
    oversized.available_panels.push_back(
        {.panel_id = std::move(id),
         .name = std::string(max_companion_text_bytes, 'n')});
  }
  layerc::test::expect(
      std::holds_alternative<CompanionError>(
          serialize_companion_snapshot_json(oversized)),
      "snapshot serializer reports the final 64 KiB wire bound");
}

static void test_companion_sidecar_survives_unrelated_bundle_save() {
  const layerc::test::TempDirectory directory("LayerC-companion-preservation");
  const auto base_path = directory.path() / "profile.json";

  GameProfile base;
  base.id = "preserve";
  base.name = "Preserve";
  Panel panel;
  panel.id = "panel";
  base.panels.push_back(std::move(panel));
  CompanionProfile companion;
  companion.profile_id = base.id;
  companion.slots = {{.slot_index = 0, .panel_id = "panel"}};
  layerc::test::expect(write_profile_bundle(base_path, base, std::nullopt,
                                            std::nullopt, companion).ok(),
                       "companion bundle should be written");

  auto changed = base;
  changed.name = "Changed";
  layerc::test::expect(write_profile_bundle(base_path, changed, std::nullopt).ok(),
                       "unrelated bundle save should succeed");
  const auto loaded = load_companion_sidecar(base_path, changed);
  layerc::test::expect(loaded.usable() &&
                           loaded.profile()->profile_id == base.id &&
                           loaded.profile()->slots.size() == 1,
                       "unrelated bundle save must preserve companion sidecar");
}

static void test_companion_profile_write_validation() {
  GameProfile base;
  base.id = "p";
  Panel panel;
  panel.id = "panel";
  base.panels.push_back(std::move(panel));
  CompanionProfile duplicate_panel;
  duplicate_panel.profile_id = "p";
  duplicate_panel.slots = {{.slot_index = 0, .panel_id = "panel"},
                           {.slot_index = 1, .panel_id = "panel"}};
  const auto error = validate_companion_profile(duplicate_panel, base);
  layerc::test::expect(error.has_value() && !(*error).message.empty(),
                       "validator rejects duplicate panel assignments");
  CompanionProfile invalid_opacity;
  invalid_opacity.profile_id = "p";
  invalid_opacity.slots = {
      {.slot_index = 0, .panel_id = "panel", .opacity = 2.0}};
  layerc::test::expect(
      validate_companion_profile(invalid_opacity, base).has_value(),
      "validator rejects out-of-range opacity");
}

static void test_gamebar_selects_multiple_game_sessions() {
  struct SelectionSession {
    bool global{};
    [[nodiscard]] bool is_global() const noexcept { return global; }
  };

  SelectionSession first;
  SelectionSession second;
  SelectionSession global{.global = true};
  std::vector<SelectionSession *> foreground{&global};
  std::vector<SelectionSession *> all{&first, &second, &global};
  const auto selected =
      layerc::daemon::select_companion_sessions<SelectionSession>(foreground,
                                                                  all, true);
  layerc::test::expect(
      selected == std::vector<SelectionSession *>{&first, &second},
      "Game Bar foreground keeps every matched game eligible instead of "
      "blanking when two sessions exist");

  const auto regular =
      layerc::daemon::select_companion_sessions<SelectionSession>(foreground,
                                                                  all, false);
  layerc::test::expect(
      regular == foreground,
      "ordinary foreground selection remains authoritative outside Game Bar");
}

void test_widget_connection_ownership();
void test_widget_service_registration_cleanup();
void test_widget_latest_state_delivery();
void test_widget_owned_subscriptions();

// Readers check shape and type only; validate_companion_snapshot owns every
// semantic bound, so a parsed snapshot and a directly constructed one report
// the same path and message for the same violation.
static void test_companion_snapshot_parse_validate_parity() {
  CompanionSessionSnapshot base;
  base.profile_id = "p";
  base.session_key = "s";
  base.timestamp_ms = 1;
  base.available_panels = {{.panel_id = "a", .name = "A"}};
  base.preferences = {CompanionSlotMapping{.slot_index = 2,
                                           .panel_id = "a",
                                           .pinned = false,
                                           .click_through = true,
                                           .opacity = 1.0}};
  CompanionPanelSnapshot panel;
  panel.panel_id = "a";
  panel.slot_index = 2;
  panel.items.push_back({.cooldown_id = "c",
                         .name = "C",
                         .progress = 0.5,
                         .available_charges = 0,
                         .max_charges = 1,
                         .remaining_ms = 10,
                         .is_ready = false});
  base.panels.push_back(panel);
  const auto serialized = serialize_companion_snapshot_json(base);
  const auto *wire = serialized_json(serialized);
  layerc::test::expect(wire != nullptr, "parity base snapshot serializes");
  if (wire == nullptr) return;

  const auto replaced = [&](const std::string_view from, const std::string &to) {
    std::string text = *wire;
    const auto at = text.find(from);
    layerc::test::expect(at != std::string::npos &&
                             text.find(from, at + 1) == std::string::npos,
                         "parity mutation targets one wire token");
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
  };
  const std::string item_json =
      R"({"availableCharges":0,"cooldownId":"c","isReady":false,"maxCharges":1,"name":"C","progress":0.5,"remainingMs":10})";
  std::string many_items = "[" + item_json;
  for (std::size_t index = 0; index < max_companion_snapshot_items; ++index)
    many_items += "," + item_json;
  many_items += "]";

  struct Case {
    const char *rule;
    std::string json;
    void (*mutate)(CompanionSessionSnapshot &);
  };
  const std::vector<Case> cases{
      {"progress within [0,1]", replaced(R"("progress":0.5)", R"("progress":1.5)"),
       [](auto &s) { s.panels[0].items[0].progress = 1.5; }},
      {"maxCharges nonzero", replaced(R"("maxCharges":1)", R"("maxCharges":0)"),
       [](auto &s) { s.panels[0].items[0].max_charges = 0; }},
      {"remainingMs nonnegative",
       replaced(R"("remainingMs":10)", R"("remainingMs":-1)"),
       [](auto &s) { s.panels[0].items[0].remaining_ms = -1; }},
      {"panel slot within eight",
       replaced(R"("panelId":"a","slotIndex":2)", R"("panelId":"a","slotIndex":8)"),
       [](auto &s) { s.panels[0].slot_index = 8; }},
      {"item count bound", replaced("[" + item_json + "]", many_items),
       [](auto &s) {
         const auto item = s.panels[0].items[0];
         s.panels[0].items.assign(max_companion_snapshot_items + 1U, item);
       }},
      {"non-empty cooldownId",
       replaced(R"("cooldownId":"c")", R"("cooldownId":"")"),
       [](auto &s) { s.panels[0].items[0].cooldown_id.clear(); }},
      {"bounded profileId",
       replaced(R"("profileId":"p")",
                "\"profileId\":\"" + std::string(max_companion_text_bytes + 1U, 'p') + "\""),
       [](auto &s) { s.profile_id.assign(max_companion_text_bytes + 1U, 'p'); }},
  };
  for (const auto &test : cases) {
    auto direct = base;
    test.mutate(direct);
    const auto expected = validate_companion_snapshot(direct);
    const auto parsed = parse_companion_snapshot_json(test.json);
    const auto *error = std::get_if<CompanionError>(&parsed);
    layerc::test::expect(
        expected && error && error->path == expected->path &&
            error->message == expected->message,
        (std::string("parsed and constructed snapshots share validation: ") +
         test.rule)
            .c_str());
  }
}

int main() {
  return layerc::test::run({
      {"test_widget_connection_ownership", test_widget_connection_ownership},
      {"test_widget_service_registration_cleanup", test_widget_service_registration_cleanup},
      {"test_widget_latest_state_delivery", test_widget_latest_state_delivery},
      {"test_widget_owned_subscriptions", test_widget_owned_subscriptions},
      {"test_companion_duplicate_slot_contract", test_companion_duplicate_slot_contract},
      {"test_companion_json_roundtrip", test_companion_json_roundtrip},
      {"test_companion_declarative_wire", test_companion_declarative_wire},
      {"test_companion_snapshot_bridge", test_companion_snapshot_bridge},
      {"test_companion_shared_pool_runtime_view", test_companion_shared_pool_runtime_view},
      {"test_companion_bridge_rejects_invalid_not_oversize", test_companion_bridge_rejects_invalid_not_oversize},
      {"test_companion_snapshot_bounds_and_session_identity", test_companion_snapshot_bounds_and_session_identity},
      {"test_bridge_truncates_worst_case_snapshot", test_bridge_truncates_worst_case_snapshot},
      {"test_all_eight_slots_persist_and_publish", test_all_eight_slots_persist_and_publish},
      {"test_companion_transport_transitions", test_companion_transport_transitions},
      {"test_companion_package_identity", test_companion_package_identity},
      {"test_companion_snapshot_rejects_malformed_input", test_companion_snapshot_rejects_malformed_input},
      {"test_companion_snapshot_parse_validate_parity", test_companion_snapshot_parse_validate_parity},
      {"test_companion_profile_write_validation", test_companion_profile_write_validation},
      {"test_companion_sidecar_survives_unrelated_bundle_save", test_companion_sidecar_survives_unrelated_bundle_save},
      {"test_gamebar_selects_multiple_game_sessions", test_gamebar_selects_multiple_game_sessions},
  });
}
