// The one panel visibility and bar presentation rule (layer45 F-D4), shared
// by the Windows and Linux overlays and the Game Bar companion snapshot.

#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace layerc::test {

void test_panel_presentation_rule() {
  using namespace layerc::core;
  using namespace std::chrono_literals;
  const std::vector<CooldownDefinition> definitions{
      {.id = "dash", .name = "Dash", .duration = 4s},
      {.id = "blink", .name = "Blink", .duration = 2s}};
  Panel bar_panel{};
  bar_panel.id = "bars";
  bar_panel.name = "Bars";
  bar_panel.layout = PanelLayout::row;
  bar_panel.style = PanelStyle::bar;
  bar_panel.anchor_x = 0.5;
  bar_panel.anchor_y = 0.5;
  bar_panel.cooldown_ids = {"dash", "missing", "blink"};
  bar_panel.bar.hide_when_ready = true;
  const auto start = TimePoint{} + 10s;

  CooldownEngine engine(definitions);
  const auto ready = engine.snapshot(start);
  auto shown = present_panel(bar_panel, definitions, ready, {}, start);
  expect(!shown.visible && shown.items.size() == 2 && shown.items[0].ready &&
             shown.items[0].remaining == 1.0 &&
             shown.items[0].label == "Dash  READY",
         "a ready hide-when-ready bar panel is hidden; unknown ids are skipped");

  expect(engine.trigger("dash", start), "dash triggers");
  const auto running = engine.snapshot(start + 1s);
  shown = present_panel(bar_panel, definitions, running, {}, start + 1s);
  expect(shown.visible && !shown.items[0].ready &&
             shown.items[0].remaining > 0.74 && shown.items[0].remaining < 0.76 &&
             shown.items[0].remaining_time == 3s &&
             shown.items[0].label == "Dash  3.0s" && shown.items[1].ready,
         "a recovering bar shows the panel with remaining fraction and label");

  shown = present_panel(bar_panel, definitions, running,
                        {.visible = false, .text = "Hold", .edit_mode = true},
                        start + 1s);
  expect(!shown.visible && shown.items[0].label == "Hold" &&
             shown.items[1].label == "Hold",
         "an automation visibility override wins over running and edit mode");
  shown = present_panel(bar_panel, definitions, ready,
                        {.visible = true, .text = {}, .edit_mode = false}, start);
  expect(shown.visible, "an override can force a ready panel visible");
  shown = present_panel(bar_panel, definitions, ready,
                        {.visible = {}, .text = {}, .edit_mode = true}, start);
  expect(shown.visible, "edit mode shows a ready panel for positioning");

  Panel card = bar_panel;
  card.style = PanelStyle::card;
  card.cooldown_ids.clear();
  shown = present_panel(card, definitions, ready, {}, start);
  expect(shown.visible && shown.items.empty(),
         "panels that do not hide when ready show even without bars");

  shown = present_panel(bar_panel, definitions, {}, {}, start);
  expect(shown.items.size() == 2 && shown.items[0].ready &&
             shown.items[0].label == "Dash",
         "a missing state presents the bar as ready under its name");

  CooldownState state{.id = "dash",
                      .available_charges = 0,
                      .max_charges = 1,
                      .next_charge_at = start,
                      .recovery_deadlines = {},
                      .recharge_duration = 4s};
  expect(!cooldown_ready(state), "a pending charge is not ready");
  state.next_charge_at.reset();
  expect(cooldown_ready(state), "no pending charge is ready");
}

} // namespace layerc::test
