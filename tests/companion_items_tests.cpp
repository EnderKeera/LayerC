// Game Bar snapshot version 2: typed rows, legacy byte stability, the exact
// 64 KiB wire boundary, oversized degradation and the widget row model.
#include "layerc/core/automation.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/companion_snapshot.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/panel_presentation.hpp"
#include "widget/WidgetRowModel.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace layerc::core;
using layerc::test::expect;

namespace {

[[nodiscard]] CompanionTypedItem typed_row(const std::string &id, const OverlayItemKind kind,
                                           const std::string &label) {
  CompanionTypedItem item;
  item.item_id = id;
  item.kind = kind;
  item.label = label;
  return item;
}

[[nodiscard]] CompanionSessionSnapshot typed_snapshot(std::vector<CompanionTypedItem> rows) {
  CompanionSessionSnapshot snapshot;
  snapshot.snapshot_version = companion_snapshot_version_typed;
  snapshot.profile_id = "typed";
  snapshot.session_key = "session";
  snapshot.timestamp_ms = 42;
  snapshot.available_panels = {{.panel_id = "main", .name = "Main"}};
  snapshot.preferences = {{.slot_index = 0, .panel_id = "main"}};
  CompanionPanelSnapshot panel;
  panel.panel_id = "main";
  panel.typed_items = std::move(rows);
  snapshot.panels.push_back(std::move(panel));
  return snapshot;
}

[[nodiscard]] std::string serialized(const CompanionSessionSnapshot &snapshot) {
  const auto result = serialize_companion_snapshot_json(snapshot);
  const auto *json = std::get_if<std::string>(&result);
  return json != nullptr ? *json : std::string{};
}

[[nodiscard]] bool parses(const std::string &json) {
  return std::holds_alternative<CompanionSessionSnapshot>(parse_companion_snapshot_json(json));
}

} // namespace

void test_companion_typed_snapshot_roundtrip() {
  auto cooldown = typed_row("dash", OverlayItemKind::cooldown, "Dash");
  cooldown.value_text = "3.0s";
  cooldown.fill = 0.25;
  cooldown.color = 0xFFFFFF;
  auto timer = typed_row("pulse", OverlayItemKind::timer, "Pulse  \"quoted\" \\ path");
  timer.fill = 1.0;
  timer.active = true;
  auto counter = typed_row("stacks", OverlayItemKind::counter, "Stacks \xCE\xBB  5/8");
  counter.has_progress = false;
  counter.fill = 0.0;
  counter.pips = max_counter_pips;
  counter.filled_pips = 5;
  counter.color = 0;
  auto minimum = typed_row("minimum", OverlayItemKind::counter, "Minimum");
  minimum.value_text = "-9223372036854775808";
  minimum.has_progress = false;
  minimum.fill = 0.0;
  auto maximum = typed_row("maximum", OverlayItemKind::counter, "Maximum");
  maximum.value_text = "9223372036854775807";
  maximum.has_progress = false;
  maximum.fill = 0.0;
  auto indicator = typed_row("power", OverlayItemKind::indicator, "Power");
  indicator.has_progress = false;
  indicator.fill = 0.0;
  indicator.active = true;
  indicator.flash_on = false;
  auto lost = typed_row("lost", OverlayItemKind::counter, std::string(max_companion_text_bytes, 'L'));
  lost.has_progress = false;
  lost.fill = 0.0;
  lost.unavailable = true;
  auto snapshot = typed_snapshot({cooldown, timer, counter, minimum, maximum, indicator, lost});
  snapshot.panels.front().omitted_items = 3;
  snapshot.panels.front().visible = false;
  const auto json = serialized(snapshot);
  expect(json.find("\"snapshotVersion\":2") != std::string::npos,
         "version 2 snapshots name their version");
  const auto parsed = parse_companion_snapshot_json(json);
  const auto *round = std::get_if<CompanionSessionSnapshot>(&parsed);
  expect(round != nullptr && *round == snapshot,
         "typed rows, escaped and multibyte text, visibility and omitted count round-trip");

  CompanionSessionSnapshot legacy = typed_snapshot({});
  legacy.snapshot_version = companion_snapshot_version_legacy;
  legacy.panels.front().items.push_back({.cooldown_id = "dash", .name = "Dash"});
  const auto legacy_json = serialized(legacy);
  expect(!legacy_json.empty() && legacy_json.find("snapshotVersion") == std::string::npos &&
             legacy_json.find("omittedItems") == std::string::npos &&
             legacy_json.find("\"visible\"") == std::string::npos,
         "version 1 output keeps its exact cooldown-only shape");
  const auto legacy_parsed = parse_companion_snapshot_json(legacy_json);
  const auto *legacy_round = std::get_if<CompanionSessionSnapshot>(&legacy_parsed);
  expect(legacy_round != nullptr && *legacy_round == legacy &&
             legacy_round->snapshot_version == companion_snapshot_version_legacy,
         "an absent snapshotVersion parses as version 1");

  // All eight slots at all 32 rows fit with short, mixed-kind fields.
  auto maximum_items = typed_snapshot({});
  constexpr std::array kinds{OverlayItemKind::cooldown, OverlayItemKind::timer,
                             OverlayItemKind::counter, OverlayItemKind::indicator};
  for (std::uint32_t slot = 0; slot < max_companion_slots; ++slot) {
    if (slot != 0) {
      const auto id = "panel" + std::to_string(slot);
      maximum_items.available_panels.push_back({.panel_id = id, .name = id});
      maximum_items.preferences.push_back({.slot_index = slot, .panel_id = id});
      CompanionPanelSnapshot panel;
      panel.panel_id = id;
      panel.slot_index = slot;
      maximum_items.panels.push_back(std::move(panel));
    }
    for (std::size_t index = 0; index < max_companion_snapshot_items; ++index) {
      auto row = typed_row("row" + std::to_string(index), kinds[index % kinds.size()], "Item");
      row.value_text = "5";
      row.has_progress = row.kind == OverlayItemKind::cooldown || row.kind == OverlayItemKind::timer;
      if (row.kind == OverlayItemKind::counter) { row.pips = 8; row.filled_pips = 5; }
      maximum_items.panels.back().typed_items.push_back(std::move(row));
    }
  }
  const auto maximum_json = serialized(maximum_items);
  const auto maximum_parsed = parse_companion_snapshot_json(maximum_json);
  const auto *maximum_round = std::get_if<CompanionSessionSnapshot>(&maximum_parsed);
  expect(!maximum_json.empty() && maximum_json.size() <= max_companion_snapshot_bytes &&
             maximum_round != nullptr && *maximum_round == maximum_items,
         "all 8x32 mixed-kind rows round-trip below the wire cap with short fields");
}

void test_companion_typed_snapshot_rejects_invalid() {
  const auto valid = typed_snapshot({typed_row("a", OverlayItemKind::counter, "A")});
  const auto json = serialized(valid);
  expect(!json.empty() && parses(json), "the fixture is valid");
  const auto replaced = [&](const std::string &from, const std::string &to) {
    auto text = json;
    const auto at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
  };
  expect(!parses(replaced("\"kind\":\"counter\"", "\"kind\":\"gauge\"")),
         "an unknown wire kind rejects the whole snapshot");
  expect(!parses(replaced("\"snapshotVersion\":2", "\"snapshotVersion\":3")),
         "an unsupported snapshot version is rejected");
  expect(!parses(replaced("\"omittedItems\":0,", "")),
         "version 2 panels require omittedItems");

  const auto invalid = [](auto mutate) {
    auto snapshot = typed_snapshot({typed_row("a", OverlayItemKind::counter, "A")});
    mutate(snapshot);
    return validate_companion_snapshot(snapshot).has_value();
  };
  expect(invalid([](auto &s) { s.panels[0].typed_items[0].fill = 1.5; }), "fill above 1");
  expect(invalid([](auto &s) {
           s.panels[0].typed_items[0].fill = std::numeric_limits<double>::quiet_NaN();
         }), "non-finite fill");
  expect(invalid([](auto &s) { s.panels[0].typed_items[0].pips = max_counter_pips + 1; }),
         "more than eight pips");
  expect(invalid([](auto &s) {
           s.panels[0].typed_items[0].pips = 2;
           s.panels[0].typed_items[0].filled_pips = 3;
         }), "filled pips beyond pips");
  expect(invalid([](auto &s) { s.panels[0].typed_items[0].color = 0x1000000; }),
         "a color beyond 0xRRGGBB");
  expect(invalid([](auto &s) { s.panels[0].typed_items[0].item_id.clear(); }), "an empty itemId");
  expect(invalid([](auto &s) {
           s.panels[0].typed_items[0].label.assign(max_companion_text_bytes + 1, 'x');
         }), "over-bound label text");
  expect(invalid([](auto &s) {
           s.panels[0].typed_items[0].value_text.assign(max_companion_text_bytes + 1, 'x');
         }), "over-bound formatted value text");
  expect(invalid([](auto &s) {
           s.available_panels[0].name.assign(max_companion_text_bytes + 1, 'x');
         }), "over-bound catalog metadata");
  expect(invalid([](auto &s) {
           s.panels[0].typed_items.push_back(s.panels[0].typed_items[0]);
         }), "a duplicate itemId");
  expect(invalid([](auto &s) {
           for (std::size_t i = 0; i <= max_companion_snapshot_items; ++i)
             s.panels[0].typed_items.push_back(
                 typed_row("r" + std::to_string(i), OverlayItemKind::counter, "R"));
         }), "more than 32 typed rows");
  expect(invalid([](auto &s) { s.panels[0].items.push_back({.cooldown_id = "a", .name = "A"}); }),
         "legacy rows inside a version 2 panel");
  expect(invalid([](auto &s) { s.snapshot_version = companion_snapshot_version_legacy; }),
         "typed rows inside a version 1 snapshot");
}

void test_companion_typed_wire_boundary() {
  constexpr auto cap = max_companion_snapshot_bytes;
  auto snapshot = typed_snapshot({});
  // Fill slots with maximum-length rows until one more row would not fit.
  bool full = false;
  for (std::uint32_t slot = 0; slot < max_companion_slots && !full; ++slot) {
    if (slot != 0) {
      const auto id = "panel" + std::to_string(slot);
      snapshot.available_panels.push_back({.panel_id = id, .name = id});
      snapshot.preferences.push_back({.slot_index = slot, .panel_id = id});
      CompanionPanelSnapshot panel;
      panel.panel_id = id;
      panel.slot_index = slot;
      snapshot.panels.push_back(std::move(panel));
    }
    auto &rows = snapshot.panels.back().typed_items;
    for (std::size_t index = 0; index < max_companion_snapshot_items; ++index) {
      rows.push_back(typed_row("row" + std::to_string(index), OverlayItemKind::counter,
                               std::string(max_companion_text_bytes, 'x')));
      if (serialized(snapshot).empty()) {
        rows.pop_back();
        full = true;
        break;
      }
    }
  }
  // Each appended ASCII byte adds exactly one wire byte: tune one string to
  // land on the cap. An empty-label row fits if the gap allows; otherwise the
  // first catalog name (up to 512 bytes) absorbs the remaining gap.
  auto &tail = snapshot.panels.back().typed_items;
  tail.push_back(typed_row("tune", OverlayItemKind::counter, ""));
  std::string *knob = &tail.back().label;
  if (serialized(snapshot).empty()) {
    tail.pop_back();
    knob = &snapshot.available_panels.front().name;
  }
  const auto size = serialized(snapshot).size();
  if (size != 0 && size <= cap && knob->size() + (cap - size) <= max_companion_text_bytes)
    knob->append(cap - size, 'z');
  const auto at_cap = serialized(snapshot);
  expect(full && at_cap.size() == cap && parses(at_cap),
         "a 65536-byte version 2 snapshot serializes and parses");
  expect(knob->size() < max_companion_text_bytes,
         "the boundary fixture leaves room to exercise a one-byte overflow");
  if (at_cap.size() == cap && knob->size() < max_companion_text_bytes) {
    knob->push_back('!');
    expect(std::holds_alternative<CompanionError>(serialize_companion_snapshot_json(snapshot)),
           "a 65537-byte version 2 snapshot is refused, never truncated");
  }
  expect(!parses(at_cap + " "), "the parser refuses a 65537-byte document");
}

void test_companion_typed_producer_and_degradation() {
  using namespace std::chrono_literals;
  const std::vector<CooldownDefinition> definitions{
      {.id = "dash", .name = "Dash", .duration = 5s}};
  CooldownEngine engine(definitions, {});
  const TimePoint start{};
  (void)engine.trigger("dash", start);
  const auto states = engine.snapshot(start + 1s);

  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.items = std::vector<OverlayItem>{
      {.id = "dash-row", .content = CooldownItem{"dash"}},
      {.id = "stacks", .label = std::string("Stacks \xCE\xBB"),
       .content = CounterItem{.variable = "stacks", .max = 8, .display = CounterDisplay::pips}},
      {.id = "power", .content = IndicatorItem{"power", 2.0}},
      {.id = "pulse", .content = AutomationTimerItem{"missing"}},
      {.id = "stopped", .content = AutomationTimerItem{"idle"}}};
  const std::vector<Panel> panels{panel};
  AutomationRuntimeSnapshot automation;
  automation.variables.emplace("stacks", AutomationValue(std::int64_t{3}));
  automation.variables.emplace("power", AutomationValue(true));
  automation.declared_timer_ids = {"idle"};

  const auto built = build_companion_snapshot_report(
      "typed", "session", panels, definitions, states, std::nullopt, start + 1s,
      {.automation = &automation, .presentation_epoch = start});
  const auto parsed = parse_companion_snapshot_json(built.json);
  const auto *snapshot = std::get_if<CompanionSessionSnapshot>(&parsed);
  if (!expect(snapshot != nullptr && snapshot->snapshot_version == companion_snapshot_version_typed &&
                  snapshot->panels.size() == 1 && built.omitted_items == 0,
              "a typed profile publishes version 2")) return;
  const auto &rows = snapshot->panels.front().typed_items;
  if (!expect(rows.size() == 5, "every typed row is carried")) return;
  expect(rows[0].kind == OverlayItemKind::cooldown && rows[0].has_progress &&
             rows[0].fill > 0.0 && rows[0].fill < 1.0,
         "the cooldown row carries its native fill fraction");
  expect(rows[1].kind == OverlayItemKind::counter && rows[1].pips == 8 &&
             rows[1].filled_pips == 3 && rows[1].label.find("Stacks \xCE\xBB") == 0,
         "counter pips and the multibyte label are resolved by LayerD");
  expect(rows[2].kind == OverlayItemKind::indicator && rows[2].active && !rows[2].unavailable,
         "the indicator is active from the automation snapshot");
  expect(rows[3].kind == OverlayItemKind::timer && rows[3].unavailable,
         "an undeclared timer source is an unavailable row, not dropped");
  expect(rows[4].kind == OverlayItemKind::timer && !rows[4].unavailable && !rows[4].active,
         "a declared-but-stopped timer stays a stopped row");

  // Flash phase comes from the producer: 2 Hz toggles every 250 ms.
  const auto later = build_companion_snapshot_report(
      "typed", "session", panels, definitions, states, std::nullopt, start + 1s + 250ms,
      {.automation = &automation, .presentation_epoch = start});
  const auto later_parsed = parse_companion_snapshot_json(later.json);
  const auto *later_snapshot = std::get_if<CompanionSessionSnapshot>(&later_parsed);
  expect(later_snapshot != nullptr &&
             later_snapshot->panels.front().typed_items[2].flash_on != rows[2].flash_on,
         "consecutive phases publish opposite flash states");

  // Legacy-only profiles keep the unchanged version 1 wire.
  Panel legacy;
  legacy.id = "legacy";
  legacy.cooldown_ids = {"dash"};
  const std::vector<Panel> legacy_panels{legacy};
  const auto old_wire = build_companion_snapshot("typed", "session", legacy_panels, definitions,
                                                 states, std::nullopt, start + 1s);
  const auto new_wire = build_companion_snapshot_report(
      "typed", "session", legacy_panels, definitions, states, std::nullopt, start + 1s,
      {.automation = &automation, .presentation_epoch = start});
  expect(!old_wire.empty() && old_wire == new_wire.json &&
             old_wire.find("snapshotVersion") == std::string::npos,
         "cooldown-only profiles publish byte-identical version 1 snapshots");

  // Eight mapped panels of 32 maximum-length typed rows cannot fit 64 KiB:
  // the tail sheds rows, counts them, and the result stays within the cap.
  std::vector<Panel> heavy;
  CompanionProfile companion;
  companion.profile_id = "typed";
  for (std::uint32_t slot = 0; slot < max_companion_slots; ++slot) {
    Panel big;
    big.id = "panel" + std::to_string(slot);
    big.name = std::string(max_companion_text_bytes, 'N');
    std::vector<OverlayItem> items;
    for (std::size_t index = 0; index < max_companion_snapshot_items; ++index)
      items.push_back({.id = "row" + std::to_string(index),
                       .label = std::string(max_companion_text_bytes - 8, 'x'),
                       .content = CooldownItem{"dash"}});
    big.items = std::move(items);
    heavy.push_back(std::move(big));
    companion.slots.push_back({.slot_index = slot, .panel_id = "panel" + std::to_string(slot)});
  }
  const auto shed = build_companion_snapshot_report(
      "typed", "session", heavy, definitions, states, companion, start + 1s,
      {.automation = &automation, .presentation_epoch = start});
  const auto shed_parsed = parse_companion_snapshot_json(shed.json);
  const auto *shed_snapshot = std::get_if<CompanionSessionSnapshot>(&shed_parsed);
  expect(!shed.json.empty() && shed.json.size() <= max_companion_snapshot_bytes &&
             shed.omitted_items > 0 && shed_snapshot != nullptr &&
             shed_snapshot->panels.size() == max_companion_slots &&
             shed_snapshot->panels.back().omitted_items == max_companion_snapshot_items &&
             shed_snapshot->panels.front().omitted_items == 0,
         "oversized typed snapshots shed rows from the last panel and report them");
  expect(shed_snapshot != nullptr && shed_snapshot->preferences.size() == max_companion_slots,
         "shedding never removes a slot mapping");

  // A legal maximum-length label plus its formatted value is kept in two
  // independently bounded fields, including escaped and multibyte names.
  std::string multibyte;
  for (std::size_t index = 0; index < max_companion_text_bytes / 2; ++index)
    multibyte += "\xCE\xBB";
  const std::array labels{std::string(max_companion_text_bytes, 'x'),
                           std::string(max_companion_text_bytes, '"'), multibyte};
  for (const auto &label : labels) {
    for (auto &big : heavy) {
      for (std::size_t index = 0; index < big.items->size(); ++index) {
        auto &item = (*big.items)[index];
        item.label = label;
        switch (index % 4) {
        case 0: item.content = CooldownItem{"dash"}; break;
        case 1: item.content = AutomationTimerItem{"missing"}; break;
        case 2: item.content = CounterItem{.variable = "stacks", .max = 8,
                                           .display = CounterDisplay::pips}; break;
        default: item.content = IndicatorItem{"power", 2.0}; break;
        }
      }
    }
    const auto bounded = build_companion_snapshot_report(
        "typed", "session", heavy, definitions, states, companion, start + 1s,
        {.automation = &automation, .presentation_epoch = start});
    const auto bounded_parsed = parse_companion_snapshot_json(bounded.json);
    const auto *bounded_snapshot = std::get_if<CompanionSessionSnapshot>(&bounded_parsed);
    if (!expect(!bounded.json.empty() && bounded.json.size() <= max_companion_snapshot_bytes &&
                    bounded.omitted_items > 0 && bounded_snapshot != nullptr &&
                    bounded_snapshot->panels.size() == max_companion_slots &&
                    bounded_snapshot->panels.front().typed_items.size() == max_companion_snapshot_items,
                "8x32 mixed rows with 512-byte ASCII, escaped or multibyte labels shed safely"))
      continue;
    const auto &first = bounded_snapshot->panels.front().typed_items.front();
    expect(first.label == label && !first.value_text.empty(),
           "maximum-length labels and formatted values survive without UTF-8 truncation");
  }

  automation.overlay_overrides.push_back(
      {.name = "item-text", .owner_automation = "test", .panel_id = "main",
       .item_id = "stacks", .property = "text",
       .value = AutomationValue(std::string("Replacement"))});
  const auto overridden = build_companion_snapshot_report(
      "typed", "session", panels, definitions, states, std::nullopt, start + 1s,
      {.automation = &automation, .presentation_epoch = start});
  const auto overridden_parsed = parse_companion_snapshot_json(overridden.json);
  const auto *overridden_snapshot = std::get_if<CompanionSessionSnapshot>(&overridden_parsed);
  expect(overridden_snapshot != nullptr &&
             overridden_snapshot->panels.front().typed_items[1].label == "Replacement" &&
             overridden_snapshot->panels.front().typed_items[1].value_text.empty(),
         "item text overrides replace the whole row, without re-appending the value");
}

void test_companion_widget_rows() {
  using layerc::gamebar::widget::panel_model;
  CompanionPanelSnapshot legacy;
  legacy.items = {{.cooldown_id = "a", .name = "Ready one", .progress = 1.0},
                  {.cooldown_id = "b", .name = "Charging", .progress = 0.4,
                   .available_charges = 1, .max_charges = 2, .remaining_ms = 2001,
                   .is_ready = false}};
  const auto old_rows = panel_model(legacy, companion_snapshot_version_legacy);
  expect(old_rows.rows.size() == 2 && old_rows.rows[0].value == "Ready" &&
             old_rows.rows[1].value == "3 s  \xC2\xB7  1/2" && old_rows.rows[1].fill == 0.4 &&
             old_rows.status.empty(),
         "version 1 rows keep their text and progress");
  expect(panel_model(CompanionPanelSnapshot{}, companion_snapshot_version_legacy).status ==
             "No cooldowns in this panel.",
         "an empty version 1 panel says so");

  CompanionPanelSnapshot typed;
  auto counter = typed_row("stacks", OverlayItemKind::counter, "Stacks");
  counter.value_text = "5/8";
  counter.has_progress = false;
  counter.pips = 8;
  counter.filled_pips = 5;
  counter.color = 0x38A97C;
  auto indicator = typed_row("power", OverlayItemKind::indicator, "Power");
  indicator.has_progress = false;
  indicator.active = true;
  auto lost = typed_row("lost", OverlayItemKind::cooldown, "Lost  Unavailable");
  lost.unavailable = true;
  typed.typed_items = {counter, indicator, lost};
  typed.omitted_items = 2;
  auto on = panel_model(typed, companion_snapshot_version_typed);
  expect(on.rows.size() == 3 && on.rows[0].label == "Stacks" && on.rows[0].value == "5/8" &&
             on.rows[0].pips == 8 && on.rows[0].filled_pips == 5 && !on.rows[0].fill && on.rows[0].color == 0x38A97CU && on.rows[1].indicator &&
             on.rows[1].accent_on && on.rows[2].unavailable && !on.rows[2].fill,
         "typed rows draw pips, accents and unavailable rows from the snapshot");
  expect(on.status == "2 item rows omitted to fit the Game Bar snapshot limit.",
         "omitted rows are visible in the widget status");
  typed.typed_items[1].flash_on = false;
  const auto off = panel_model(typed, companion_snapshot_version_typed);
  expect(off.rows[1].indicator && !off.rows[1].accent_on && off.rows.size() == on.rows.size(),
         "the off phase keeps the indicator row and its swatch space");
  typed.visible = false;
  const auto hidden = panel_model(typed, companion_snapshot_version_typed);
  expect(hidden.rows.empty() && hidden.status == "Hidden by this profile right now.",
         "a hidden panel shows no stale rows");
}

// Producer-owned presentation cadence: the Game Bar snapshot carries its own
// next update instead of depending on native overlay scheduling.
void test_companion_presentation_schedule() {
  using namespace std::chrono_literals;
  const TimePoint start{};
  const std::vector<CooldownDefinition> no_definitions;
  const std::vector<CooldownDefinition> definitions{
      {.id = "dash", .name = "Dash", .duration = 5s}};

  const auto indicator_build = [&](const AutomationValue truth, const double hz,
                                   const TimePoint at) {
    Panel panel;
    panel.id = "main";
    panel.name = "Main";
    panel.items = std::vector<OverlayItem>{
        {.id = "power", .content = IndicatorItem{"power", hz}}};
    AutomationRuntimeSnapshot automation;
    automation.variables.emplace("power", truth);
    const std::vector<Panel> panels{panel};
    return build_companion_snapshot_report(
        "typed", "session", panels, no_definitions, {}, std::nullopt, at,
        {.automation = &automation, .presentation_epoch = start});
  };
  for (int step = 0; step < 8; ++step) {
    const auto at = start + std::chrono::milliseconds{step * 125};
    const auto built = indicator_build(AutomationValue(true), 4.0, at);
    const auto parsed = parse_companion_snapshot_json(built.json);
    const auto *snapshot = std::get_if<CompanionSessionSnapshot>(&parsed);
    if (!expect(snapshot != nullptr && snapshot->panels.size() == 1 &&
                    snapshot->panels.front().typed_items.size() == 1,
                "a 4 Hz indicator publishes one typed row per producer edge"))
      return;
    expect(snapshot->panels.front().typed_items.front().flash_on == (step % 2 == 0) &&
               built.next_wakeup == at + std::chrono::milliseconds{125},
           "eight 4 Hz producer edges alternate phase and request the next edge");
  }

  expect(!indicator_build(AutomationValue(true), 0.0, start).next_wakeup,
         "a steady indicator requests no producer timer");
  expect(!indicator_build(AutomationValue(false), 4.0, start).next_wakeup,
         "an inactive indicator requests no producer timer");

  {
    Panel panel;
    panel.id = "main";
    panel.name = "Main";
    panel.items = std::vector<OverlayItem>{
        {.id = "power", .content = IndicatorItem{"power", 4.0}}};
    AutomationRuntimeSnapshot automation;
    automation.variables.emplace("power", AutomationValue(true));
    automation.overlay_overrides.push_back(
        {.name = "hide", .owner_automation = "test", .panel_id = "main",
         .property = "visible", .value = AutomationValue(false)});
    const std::vector<Panel> panels{panel};
    const auto built = build_companion_snapshot_report(
        "typed", "session", panels, no_definitions, {}, std::nullopt, start,
        {.automation = &automation, .presentation_epoch = start});
    expect(!built.next_wakeup,
           "a panel hidden by visibility override requests no producer timer");
  }
  {
    Panel panel;
    panel.id = "main";
    panel.name = "Main";
    panel.items = std::vector<OverlayItem>{
        {.id = "power", .content = IndicatorItem{"power", 4.0}}};
    AutomationRuntimeSnapshot automation;
    automation.variables.emplace("power", AutomationValue(true));
    const std::vector<Panel> panels{panel};
    CompanionProfile other;
    other.profile_id = "typed";
    other.slots.push_back({.slot_index = 0, .panel_id = "unmapped"});
    const auto built = build_companion_snapshot_report(
        "typed", "session", panels, no_definitions, {}, other, start,
        {.automation = &automation, .presentation_epoch = start});
    expect(!built.next_wakeup, "an unmapped panel requests no producer timer");
  }
  {
    const std::vector<Panel> no_panels;
    const auto built = build_companion_snapshot_report(
        "typed", "session", no_panels, no_definitions, {}, std::nullopt, start, {});
    expect(!built.next_wakeup, "a session without panels requests no producer timer");
  }
  {
    Panel panel;
    panel.id = "main";
    panel.name = "Main";
    panel.items = std::vector<OverlayItem>{
        {.id = "stacks",
         .content = CounterItem{"stacks", 8.0, CounterDisplay::number}}};
    AutomationRuntimeSnapshot automation;
    automation.variables.emplace("stacks", AutomationValue(std::int64_t{3}));
    const std::vector<Panel> panels{panel};
    const auto built = build_companion_snapshot_report(
        "typed", "session", panels, no_definitions, {}, std::nullopt, start,
        {.automation = &automation, .presentation_epoch = start});
    expect(!built.next_wakeup, "a static counter panel requests no producer timer");
  }
  {
    const auto timer_build = [&](const TimePoint now, const TimePoint due) {
      Panel panel;
      panel.id = "main";
      panel.name = "Main";
      panel.items = std::vector<OverlayItem>{
          {.id = "tick", .content = AutomationTimerItem{"tick"}}};
      AutomationRuntimeSnapshot automation;
      automation.declared_timer_ids = {"tick"};
      automation.timers.push_back(
          {.id = "tick", .owner_automation = "a", .due = due, .interval = 5s,
           .repeating = false});
      const std::vector<Panel> panels{panel};
      return build_companion_snapshot_report(
          "typed", "session", panels, no_definitions, {}, std::nullopt, now,
          {.automation = &automation, .presentation_epoch = start});
    };
    expect(timer_build(start, start + 5s).next_wakeup ==
               start + std::chrono::milliseconds{100},
           "a running countdown refreshes its text at most 100 ms later");
    expect(timer_build(start, start + 50ms).next_wakeup == start + 50ms,
           "a countdown expiring before 100 ms keeps its exact expiry");
  }
  {
    // Shedding clears whole panels from the tail, so the only panel that
    // carried a live countdown must not resurrect a deadline once omitted.
    CooldownEngine engine(definitions, {});
    (void)engine.trigger("dash", start);
    const auto states = engine.snapshot(start + 1s);
    std::string multibyte;
    for (std::size_t index = 0; index < max_companion_text_bytes / 2; ++index)
      multibyte += "\xCE\xBB";
    AutomationRuntimeSnapshot automation;
    automation.variables.emplace("stacks", AutomationValue(std::int64_t{3}));
    std::vector<Panel> panels;
    CompanionProfile mapping;
    mapping.profile_id = "typed";
    for (std::uint32_t slot = 0; slot < max_companion_slots; ++slot) {
      Panel big;
      big.id = "panel" + std::to_string(slot);
      big.name = multibyte;
      std::vector<OverlayItem> items;
      const bool live = slot == max_companion_slots - 1;
      for (std::size_t index = 0; index < max_companion_snapshot_items; ++index) {
        OverlayItem row;
        row.id = "row" + std::to_string(index);
        row.label = multibyte;
        row.content = live ? OverlayItemContent{CooldownItem{"dash"}}
                           : OverlayItemContent{CounterItem{
                                 "stacks", 8.0, CounterDisplay::number}};
        items.push_back(std::move(row));
      }
      big.items = std::move(items);
      panels.push_back(std::move(big));
      mapping.slots.push_back({.slot_index = slot,
                               .panel_id = "panel" + std::to_string(slot)});
    }
    const auto full = build_companion_snapshot_report(
        "typed", "session", panels, definitions, states, mapping, start + 1s,
        {.automation = &automation, .presentation_epoch = start});
    const auto full_parsed = parse_companion_snapshot_json(full.json);
    const auto *full_snapshot = std::get_if<CompanionSessionSnapshot>(&full_parsed);
    expect(!full.json.empty() && full.omitted_items > 0 && full_snapshot != nullptr &&
               full_snapshot->panels.back().typed_items.empty() && !full.next_wakeup,
           "a shed panel that carried the only countdown leaves no producer deadline");
  }
}

// Legacy cooldown-only panels must still publish real item-scoped automation
// effects: a targetable @legacy:<index> effect promotes the snapshot to V2,
// while a wire without such a feature keeps its exact version-1 bytes.
void test_companion_legacy_item_overrides() {
  using namespace std::chrono_literals;
  const TimePoint start{};
  const auto now = start + 1s;
  const std::vector<CooldownDefinition> definitions{
      {.id = "dash", .name = "Dash", .duration = 5s},
      {.id = "shield", .name = "Shield", .duration = 10s}};
  CooldownEngine engine(definitions, {});
  (void)engine.trigger("dash", start);
  const auto states = engine.snapshot(now);
  Panel legacy;
  legacy.id = "legacy";
  legacy.name = "Legacy";
  legacy.cooldown_ids = {"dash", "shield"};
  const std::vector<Panel> panels{legacy};
  const auto baseline = build_companion_snapshot(
      "legacy", "session", panels, definitions, states, std::nullopt, now);
  AutomationRuntimeSnapshot automation;
  automation.variables.emplace("unrelated", AutomationValue(true));
  const auto publish = [&] {
    return build_companion_snapshot_report(
        "legacy", "session", panels, definitions, states, std::nullopt, now,
        {.automation = &automation, .presentation_epoch = start});
  };
  const auto unchanged = publish();
  expect(!baseline.empty() && unchanged.json == baseline &&
             baseline.find("snapshotVersion") == std::string::npos &&
             !unchanged.next_wakeup,
         "legacy-only panels without item effects retain the exact V1 bytes and idle cadence");

  struct OverrideCase {
    std::string property;
    AutomationValue value;
  };
  const std::array cases{
      OverrideCase{"visible", AutomationValue(false)},
      OverrideCase{"text", AutomationValue(std::string("Replacement"))},
      OverrideCase{"color", AutomationValue(std::int64_t{0x123456})}};
  for (const auto &test : cases) {
    automation.overlay_overrides = {
        {.name = "legacy-item", .owner_automation = "test", .panel_id = "legacy",
         .item_id = "@legacy:0", .property = test.property, .value = test.value}};
    const auto built = publish();
    const auto parsed = parse_companion_snapshot_json(built.json);
    const auto *snapshot = std::get_if<CompanionSessionSnapshot>(&parsed);
    if (!expect(snapshot != nullptr &&
                    snapshot->snapshot_version == companion_snapshot_version_typed &&
                    snapshot->panels.size() == 1 && snapshot->panels[0].items.empty(),
                "a valid legacy item effect selects V2 without adding a typed panel: " + test.property))
      continue;
    const auto &shown = snapshot->panels[0];
    const auto resolved = resolve_panel_overrides(legacy, automation.overlay_overrides);
    PanelPresentationOverrides options;
    options.visible = resolved.visible;
    options.text = resolved.text;
    options.items = resolved.items;
    options.presentation_epoch = start;
    const auto native = present_panel(resolved.panel, definitions, states,
                                      &automation, options, now);
    expect(shown.visible == native.visible && shown.typed_items.size() == native.items.size(),
           "Game Bar preserves native legacy item visibility: " + test.property);
    if (test.property == "visible") {
      expect(shown.typed_items.size() == 1 &&
                 shown.typed_items[0].item_id == "@legacy:1" && !built.next_wakeup,
             "hiding the running legacy row keeps its sibling and removes its wakeup");
    } else if (test.property == "text") {
      expect(shown.typed_items.size() == 2 &&
                 shown.typed_items[0].item_id == "@legacy:0" &&
                 shown.typed_items[0].label == "Replacement" &&
                 shown.typed_items[0].value_text.empty() &&
                 shown.typed_items[1].item_id == "@legacy:1" &&
                 shown.typed_items[1].label == "Shield",
             "legacy item text replaces the whole target row, not its sibling");
      expect(built.next_wakeup == now + 100ms,
             "a promoted legacy running row keeps the producer countdown deadline");
    } else {
      expect(shown.typed_items.size() == 2 && native.items.size() == 2 &&
                 shown.typed_items[0].color == 0x123456U &&
                 shown.typed_items[0].color == (native.items[0].color & 0xFFFFFFU) &&
                 shown.typed_items[1].color == (native.items[1].color & 0xFFFFFFU),
             "legacy item color matches native presentation and leaves its sibling unchanged");
    }
    const auto widget = layerc::gamebar::widget::panel_model(
        shown, companion_snapshot_version_typed);
    expect(widget.rows.size() == native.items.size(),
           "the widget consumes the legacy item effect: " + test.property);
    if (test.property == "text" && !widget.rows.empty())
      expect(widget.rows[0].label == "Replacement" && widget.rows[0].value.empty(),
             "the widget does not append stale text to a legacy text replacement");
    if (test.property == "color" && !widget.rows.empty())
      expect(widget.rows[0].color == 0x123456U,
             "the widget receives the legacy item's resolved color");

    auto mixed = panels;
    Panel unrelated;
    unrelated.id = "unrelated";
    unrelated.items = std::vector<OverlayItem>{};
    mixed.push_back(unrelated);
    const auto mixed_build = build_companion_snapshot_report(
        "legacy", "session", mixed, definitions, states, std::nullopt, now,
        {.automation = &automation, .presentation_epoch = start});
    const auto mixed_parsed = parse_companion_snapshot_json(mixed_build.json);
    const auto *mixed_snapshot = std::get_if<CompanionSessionSnapshot>(&mixed_parsed);
    expect(mixed_snapshot != nullptr && !mixed_snapshot->panels.empty() &&
               mixed_snapshot->panels[0] == shown,
           "adding an unrelated typed panel cannot change a legacy item effect: " + test.property);
  }

  for (const auto &target : std::array{
           std::pair{std::string("foreign"), std::string("@legacy:0")},
           std::pair{std::string("legacy"), std::string("@legacy:2")}}) {
    automation.overlay_overrides = {
        {.name = "ignored", .owner_automation = "test", .panel_id = target.first,
         .item_id = target.second, .property = "visible", .value = AutomationValue(false)}};
    expect(publish().json == baseline,
           "an unrelated or out-of-range target does not promote the legacy wire");
  }
  automation.overlay_overrides.clear();
  expect(publish().json == baseline,
         "clearing the final item effect restores the exact legacy V1 wire");
}
