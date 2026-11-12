#include "overlay_service.hpp"
#include "x11_provider.hpp"
#include "x11_window.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/panel_presentation.hpp"
#include "layerc/platform/automation_persistence.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include "test_support.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using layerc::core::AutomationAction;
using layerc::core::AutomationActionKind;
using layerc::core::AutomationEvent;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationProfile;
using layerc::core::AutomationRuntimeSnapshot;
using layerc::core::AutomationValue;
using layerc::core::AutomationValueType;
using layerc::core::GameProfile;
using layerc::core::ItemPresentation;
using layerc::core::OverlayItem;
using layerc::core::OverlayItemKind;
using layerc::core::Panel;
using layerc::core::TimePoint;
using layerc::platform::OverlayBar;
using layerc::platform::OverlayDisplayList;
using layerc::platform::OverlayFrame;
using layerc::platform::OverlayOp;
using layerc::platform::OverlayOpKind;

[[nodiscard]] layerc::core::AutomationExpression
literal(AutomationValue value) {
  layerc::core::AutomationExpression expression;
  expression.kind = layerc::core::ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

[[nodiscard]] ItemPresentation row(std::string item_id, std::string label,
                                   const OverlayItemKind kind,
                                   const std::uint32_t color) {
  ItemPresentation result;
  result.item_id = std::move(item_id);
  result.name = result.item_id;
  result.label = std::move(label);
  result.kind = kind;
  result.color = color;
  return result;
}

[[nodiscard]] OverlayFrame paint_frame(
    std::vector<ItemPresentation> rows,
    const layerc::core::PanelStyle style = layerc::core::PanelStyle::bar,
    const std::uint32_t width = 320, const std::uint32_t height = 48) {
  OverlayFrame frame;
  frame.target = {20, 30, 800, 600};
  frame.panel.id = "typed";
  frame.panel.name = "Typed";
  frame.panel.style = style;
  frame.panel.bar.width = width;
  frame.panel.bar.height = height;
  frame.panel.bar.background_color = 0x272D35U;
  frame.panel.bar.border_color = 0x50BAC8U;
  frame.dpi = layerc::platform::baseline_dpi;
  frame.bars.reserve(rows.size());
  for (const auto &item : rows)
    frame.bars.push_back(layerc::platform::overlay_bar_from_presentation(item));
  return frame;
}

[[nodiscard]] bool has_op(const OverlayDisplayList &list,
                          const OverlayOpKind kind,
                          const std::uint32_t color) {
  return std::ranges::any_of(list.ops, [&](const OverlayOp &op) {
    return op.kind == kind && op.rgb == color;
  });
}

[[nodiscard]] const OverlayOp *find_op(const OverlayDisplayList &list,
                                       const OverlayOpKind kind,
                                       const std::uint32_t color) {
  const auto found = std::ranges::find_if(list.ops, [&](const OverlayOp &op) {
    return op.kind == kind && op.rgb == color;
  });
  return found == list.ops.end() ? nullptr : &*found;
}

[[nodiscard]] Panel typed_profile_panel() {
  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.style = layerc::core::PanelStyle::bar;
  panel.bar.width = 360;
  panel.bar.height = 40;
  panel.bar.hide_when_ready = true;
  OverlayItem low;
  low.id = "low";
  low.label = "Minimum";
  low.content = layerc::core::CounterItem{"minimum", std::nullopt,
                                          layerc::core::CounterDisplay::number};
  OverlayItem high;
  high.id = "high";
  high.label = "Maximum";
  high.content = layerc::core::CounterItem{"maximum", std::nullopt,
                                           layerc::core::CounterDisplay::number};
  OverlayItem missing;
  missing.id = "missing";
  missing.label = "Missing";
  missing.content = layerc::core::CounterItem{"absent", std::nullopt,
                                               layerc::core::CounterDisplay::number};
  panel.items = std::vector<OverlayItem>{low, high, missing};
  return panel;
}

void paint_contracts() {
  using layerc::test::expect;

  const auto panel = typed_profile_panel();
  AutomationRuntimeSnapshot values;
  values.variables.emplace(
      "minimum", AutomationValue(std::numeric_limits<std::int64_t>::min()));
  values.variables.emplace(
      "maximum", AutomationValue(std::numeric_limits<std::int64_t>::max()));
  layerc::core::PanelPresentationOverrides overrides;
  overrides.items.push_back({.item_id = "low", .color = 0U});
  const auto presentation = layerc::core::present_panel(
      panel, std::span<const layerc::core::CooldownDefinition>{},
      std::span<const layerc::core::CooldownState>{}, &values, overrides,
      TimePoint{});
  expect(presentation.items.size() == 3,
         "mixed typed rows retain source order including unavailable rows");
  if (presentation.items.size() == 3) {
    expect(presentation.items[0].label ==
               "Minimum  -9223372036854775808" &&
               presentation.items[0].value_text == "-9223372036854775808",
           "negative integer-extreme counter text is preserved exactly");
    expect(presentation.items[1].label ==
               "Maximum  9223372036854775807" &&
               presentation.items[1].value_text == "9223372036854775807",
           "positive integer-extreme counter text is preserved exactly");
    expect(presentation.items[2].unavailable &&
               presentation.items[2].value_text == "Unavailable" &&
               !presentation.items[2].has_progress,
           "missing counter sources remain unavailable without progress");
    expect(presentation.items[0].color == 0U,
           "an explicit item accent may be black");
  }

  OverlayFrame extreme_frame;
  extreme_frame.target = {0, 0, 800, 600};
  extreme_frame.panel = panel;
  for (const auto &item : presentation.items)
    extreme_frame.bars.push_back(
        layerc::platform::overlay_bar_from_presentation(item));
  const auto extreme_list =
      layerc::platform::build_overlay_display_list(extreme_frame);
  expect(std::ranges::any_of(extreme_list.ops, [](const OverlayOp &op) {
           return op.kind == OverlayOpKind::text &&
                  op.text == "Minimum  -9223372036854775808";
         }) &&
             std::ranges::any_of(extreme_list.ops, [](const OverlayOp &op) {
               return op.kind == OverlayOpKind::text &&
                      op.text == "Maximum  9223372036854775807";
             }),
         "the display adapter copies complete integer-extreme row labels");
  expect(!has_op(extreme_list, OverlayOpKind::fill_rect, 0U) &&
             !has_op(extreme_list, OverlayOpKind::fill_rect, panel.bar.fill_color),
         "number-only and unavailable rows fabricate no progress fill");

  auto cooldown = row("cooldown", "Dash  5.0s", OverlayItemKind::cooldown,
                      0xD04A36U);
  cooldown.remaining = 0.5;
  cooldown.ready = false;
  auto timer = row("timer", "Pulse  2.5s", OverlayItemKind::timer,
                   0xE56B44U);
  timer.remaining = 0.5;
  timer.ready = false;
  timer.active = true;
  auto counter = row("pips", "Charge  2/4", OverlayItemKind::counter,
                     0x38A97CU);
  counter.has_progress = false;
  counter.pips = 4;
  counter.filled_pips = 2;
  auto active_indicator = row("power", "Power", OverlayItemKind::indicator,
                               0U);
  active_indicator.has_progress = false;
  active_indicator.active = true;
  active_indicator.ready = false;
  auto unavailable_row = row("lost", "Lost  Unavailable",
                             OverlayItemKind::counter, 0x123456U);
  unavailable_row.unavailable = true;
  unavailable_row.has_progress = false;
  unavailable_row.pips = 8;
  unavailable_row.filled_pips = 8;
  const auto mixed = layerc::platform::build_overlay_display_list(
      paint_frame({cooldown, timer, counter, active_indicator,
                   unavailable_row}));
  expect(mixed.ops.size() <= 2 + 5 * 11 &&
             has_op(mixed, OverlayOpKind::fill_rect, cooldown.color) &&
             has_op(mixed, OverlayOpKind::fill_rect, timer.color) &&
             has_op(mixed, OverlayOpKind::fill_rect, 0U) &&
             !has_op(mixed, OverlayOpKind::fill_rect,
                     unavailable_row.color),
         "mixed cooldown, timer, counter, indicator and unavailable rows paint by kind");

  auto indicator = row("indicator", "Power", OverlayItemKind::indicator, 0U);
  indicator.active = true;
  indicator.ready = false;
  indicator.flash_on = true;
  indicator.has_progress = false;
  const auto on = layerc::platform::build_overlay_display_list(
      paint_frame({indicator}));
  indicator.flash_on = false;
  const auto off = layerc::platform::build_overlay_display_list(
      paint_frame({indicator}));
  expect(on.bounds == off.bounds,
         "indicator flash phases retain identical overlay bounds");
  const auto text = [](const OverlayDisplayList &list) -> const OverlayOp * {
    const auto found = std::ranges::find_if(list.ops, [](const OverlayOp &op) {
      return op.kind == OverlayOpKind::text;
    });
    return found == list.ops.end() ? nullptr : &*found;
  };
  const auto *on_text = text(on);
  const auto *off_text = text(off);
  expect(on_text != nullptr && off_text != nullptr &&
             on_text->text == "Power" && off_text->text == "Power" &&
             on_text->rect == off_text->rect,
         "indicator flash phases preserve label content and geometry");
  expect(has_op(on, OverlayOpKind::fill_rect, 0U) &&
             !has_op(off, OverlayOpKind::fill_rect, 0U),
         "an active indicator paints its black accent only in the on phase");

  auto empty_pips = row("empty", "0/4", OverlayItemKind::counter, 0xA13377U);
  empty_pips.has_progress = false;
  empty_pips.pips = 4;
  empty_pips.filled_pips = 0;
  const auto pip_frame = paint_frame({empty_pips});
  const auto pip_area = layerc::core::bar_geometry(
      pip_frame.panel, 0, 1, 1.0).text;
  const auto layout = layerc::core::item_pip_layout(
      pip_frame.panel.style, pip_area, empty_pips.pips);
  const auto pip_list =
      layerc::platform::build_overlay_display_list(pip_frame);
  const auto empty_strokes = static_cast<std::size_t>(std::ranges::count_if(
      pip_list.ops, [&](const OverlayOp &op) {
        return op.kind == OverlayOpKind::stroke_rect &&
               op.rgb == empty_pips.color;
      }));
  expect(layout.pips.size() == 4 && empty_strokes == layout.pips.size() &&
             !has_op(pip_list, OverlayOpKind::fill_rect, empty_pips.color),
         "empty pips remain outlines and never fabricate filled values");

  auto clamped_pips = row("clamped", "8/32", OverlayItemKind::counter,
                          0xA13377U);
  clamped_pips.has_progress = false;
  clamped_pips.pips = 32;
  clamped_pips.filled_pips = 32;
  const auto clamped_area = layerc::core::bar_geometry(
      pip_frame.panel, 0, 1, 1.0).text;
  expect(layerc::core::item_pip_layout(pip_frame.panel.style, clamped_area,
                                       clamped_pips.pips).pips.size() ==
             layerc::core::max_counter_pips,
         "pip geometry clamps authored counts to eight");

  std::vector<ItemPresentation> full_rows;
  full_rows.reserve(layerc::core::max_overlay_items_per_panel);
  for (std::size_t index = 0;
       index < layerc::core::max_overlay_items_per_panel; ++index) {
    auto item = row("counter-" + std::to_string(index), "Count  4/8",
                    OverlayItemKind::counter, 0x38A97CU);
    item.remaining = 0.5;
    item.has_progress = true;
    item.pips = layerc::core::max_counter_pips;
    item.filled_pips = 4;
    full_rows.push_back(std::move(item));
  }
  const auto full_frame = paint_frame(std::move(full_rows));
  const auto full_list =
      layerc::platform::build_overlay_display_list(full_frame);
  expect(full_list.ops.size() == 354,
         "32 rows with eight pips emit exactly 354 operations");

  for (const int dpi : {96, 144, 192}) {
    for (const auto &[width, height] :
         {std::pair{std::uint32_t{320}, std::uint32_t{48}},
          std::pair{layerc::core::min_bar_width,
                    layerc::core::min_bar_height}}) {
      auto item = row("geometry", "Count  4/8", OverlayItemKind::counter,
                      0x38A97CU);
      item.remaining = 0.5;
      item.pips = 8;
      item.filled_pips = 4;
      auto frame = paint_frame({item}, layerc::core::PanelStyle::bar, width,
                               height);
      frame.dpi = dpi;
      const auto list =
          layerc::platform::build_overlay_display_list(frame);
      expect(list.ops.size() <= 354,
             "typed geometry remains within the 354-operation cap");
      for (const auto &op : list.ops)
        expect(op.rect.width > 0 && op.rect.height > 0 && op.rect.x >= 0 &&
                   op.rect.y >= 0 && op.rect.x + op.rect.width <= list.bounds.width &&
                   op.rect.y + op.rect.height <= list.bounds.height,
               "paint primitives stay positive and inside list bounds");
    }
  }
}

struct CapturedFrame {
  std::string key;
  OverlayFrame frame;
};

class CaptureBackend final : public layerc::daemon::OverlayBackend {
public:
  bool show(std::string_view key, const OverlayFrame &frame) override {
    frames.push_back({std::string(key), frame});
    return !fail_show;
  }
  void hide(std::string_view key) noexcept override {
    hidden.emplace_back(key);
  }
  void retain(std::span<const std::string> keys) noexcept override {
    retained.emplace_back(keys.begin(), keys.end());
  }
  [[nodiscard]] std::vector<std::string> unavailable() const override {
    return {};
  }

  bool fail_show{};
  std::vector<CapturedFrame> frames;
  std::vector<std::string> hidden;
  std::vector<std::vector<std::string>> retained;
};

[[nodiscard]] layerc::platform::WindowScan scan_for(
    const std::uint64_t window = 42) {
  layerc::platform::WindowScan scan;
  scan.authoritative = true;
  scan.active = window;
  layerc::platform::WindowSnapshot snapshot;
  snapshot.window = window;
  snapshot.title = "Fixture";
  snapshot.executable_name = "fixture";
  snapshot.width = 800;
  snapshot.height = 600;
  snapshot.active = true;
  scan.windows.push_back(std::move(snapshot));
  return scan;
}

[[nodiscard]] GameProfile game_profile(std::string id, Panel panel) {
  GameProfile profile;
  profile.id = std::move(id);
  profile.name = profile.id;
  profile.game.executable_name = "fixture";
  profile.panels.push_back(std::move(panel));
  return profile;
}

[[nodiscard]] AutomationProfile counter_defaults(const std::string_view id,
                                                  const std::int64_t count) {
  AutomationProfile profile;
  profile.profile_id = id;
  profile.variables.push_back({.id = "count",
                               .type = AutomationValueType::integer,
                               .default_value = AutomationValue(count)});
  return profile;
}

void start_service(layerc::daemon::OverlayService &service,
                   CaptureBackend &backend,
                   std::span<layerc::daemon::InputSession> sessions,
                   const layerc::platform::WindowScan &scan) {
  service.use_backend(&backend);
  service.refresh(sessions, scan, {});
  layerc::runtime::CapabilityDemand demand;
  demand.set(layerc::runtime::Capability::overlay, 1);
  layerc::test::expect(
      service.reconcile(demand).status ==
          layerc::runtime::ServiceReconcileStatus::ok,
      "a demanded capture backend reconciles");
}

[[nodiscard]] const ItemPresentation *find_item(
    const OverlayFrame &frame, const std::string_view item_id) {
  const auto found = std::ranges::find_if(frame.bars, [&](const OverlayBar &bar) {
    return bar.presentation && bar.presentation->item_id == item_id;
  });
  return found == frame.bars.end() ? nullptr : &*found->presentation;
}

[[nodiscard]] AutomationAction item_override(
    std::string item_id, std::string name, std::string property,
    AutomationValue value, const std::optional<layerc::core::Milliseconds> expiry =
                               std::nullopt) {
  AutomationAction action;
  action.kind = AutomationActionKind::set_overlay_override;
  action.target = "main";
  action.name = std::move(name);
  action.property = std::move(property);
  action.item_id = std::move(item_id);
  action.value = literal(std::move(value));
  if (expiry)
    action.secondary = literal(AutomationValue(layerc::core::DurationValue{*expiry}));
  return action;
}

[[nodiscard]] AutomationProfile live_automation(const std::string_view id) {
  AutomationProfile profile = counter_defaults(id, 1);
  AutomationAction set_count;
  set_count.kind = AutomationActionKind::set_variable;
  set_count.target = "count";
  set_count.value = literal(AutomationValue(std::int64_t{7}));
  AutomationAction create_timer;
  create_timer.kind = AutomationActionKind::create_timer;
  create_timer.target = "pulse";
  create_timer.value = literal(
      AutomationValue(layerc::core::DurationValue{5000ms}));

  layerc::core::AutomationRule startup;
  startup.id = "startup";
  startup.event = AutomationEventKind::session_started;
  startup.actions = {set_count, create_timer};

  layerc::core::AutomationRule override_rule;
  override_rule.id = "override";
  override_rule.event = AutomationEventKind::input_pressed;
  override_rule.source_filter = "keyboard:1";
  const auto expiry = std::optional{500ms};
  override_rule.actions = {
      item_override("count", "emphasis", "visible", AutomationValue(true), expiry),
      item_override("count", "emphasis", "text",
                    AutomationValue(std::string("Boosted")), expiry),
      item_override("count", "emphasis", "color",
                    AutomationValue(std::int64_t{0}), expiry),
      item_override("pulse", "emphasis", "visible", AutomationValue(false), expiry)};

  AutomationAction clear;
  clear.kind = AutomationActionKind::clear_overlay_override;
  clear.target = "emphasis";
  layerc::core::AutomationRule clear_rule;
  clear_rule.id = "clear";
  clear_rule.event = AutomationEventKind::input_released;
  clear_rule.source_filter = "keyboard:1";
  clear_rule.actions = {clear};

  profile.automations.push_back({.id = "display",
                                 .name = "Display",
                                 .enabled = true,
                                 .rules = {startup, override_rule, clear_rule}});
  return profile;
}

[[nodiscard]] AutomationEvent input_event(const AutomationEventKind kind,
                                          const TimePoint at,
                                          const std::string_view source) {
  return {.kind = kind,
          .timestamp = at,
          .source_id = std::string(source),
          .origin_token = 1,
          .fields = {}};
}

void service_contracts() {
  using layerc::test::expect;

  layerc::test::TempDirectory temp{"layerc-typed-overlay-defaults"};
  layerc::platform::AutomationPersistence persistence;
  auto panel = typed_profile_panel();
  panel.items->resize(1);
  panel.items->front().id = "count";
  panel.items->front().label = "Count";
  panel.items->front().content =
      layerc::core::CounterItem{"count", 10.0,
                                layerc::core::CounterDisplay::number};
  auto base = game_profile("defaults", panel);
  auto declared = counter_defaults(base.id, 5);
  layerc::daemon::ConfiguredProfile bundle;
  bundle.source = {.path = temp.path() / "defaults.json", .profile = base};
  bundle.presentation_defaults =
      layerc::core::automation_default_snapshot(declared);
  std::vector<layerc::daemon::InputSession> sessions;
  sessions.emplace_back(bundle, 42, persistence);
  sessions.back().active = true;
  sessions.back().presentation_epoch = TimePoint{};
  const auto scan = scan_for();
  const auto now = layerc::core::MonotonicClock::now();
  CaptureBackend backend;
  layerc::daemon::OverlayService service;
  start_service(service, backend, sessions, scan);
  service.render(sessions, now);
  expect(sessions.front().automation.runtime() == nullptr &&
             sessions.front().presentation_defaults.has_value(),
         "declared defaults paint without creating an automation evaluator");
  expect(!backend.frames.empty() && find_item(backend.frames.back().frame, "count") != nullptr &&
             find_item(backend.frames.back().frame, "count")->label == "Count  5/10",
         "the no-runtime frame paints the declared counter value");
  expect(!service.next_render_deadline(now).has_value(),
         "a static counter does not arm an idle repaint poll");

  sessions.front().presentation_defaults.reset();
  service.render(sessions, now + 1ms);
  const auto *unavailable = backend.frames.empty()
      ? nullptr : find_item(backend.frames.back().frame, "count");
  expect(unavailable != nullptr && unavailable->unavailable &&
             unavailable->label == "Count  Unavailable",
         "a null defaults snapshot leaves automation-bound items unavailable");
  expect(!service.next_render_deadline(now + 1ms).has_value(),
         "an unavailable static provider has no periodic deadline");

  sessions.front().presentation_defaults =
      layerc::core::automation_default_snapshot(declared);
  sessions.front().presentation_defaults->variables["count"] =
      AutomationValue(std::int64_t{0});
  service.render(sessions, now + 2ms);
  expect(!backend.hidden.empty() && !service.next_render_deadline(now + 2ms),
         "a hidden ready provider hides without polling");

  sessions.front().presentation_defaults->variables["count"] =
      AutomationValue(std::int64_t{5});
  backend.fail_show = true;
  service.render(sessions, now + 3ms);
  const auto retry = service.next_render_deadline(now + 3ms);
  expect(retry.has_value() && *retry == now + 3ms +
                                         layerc::daemon::overlay_repaint_interval,
         "a failed demanded show retries after 100 ms");
  backend.fail_show = false;
  service.render(sessions, now + 3ms + layerc::daemon::overlay_repaint_interval);
  expect(service.window_count() == 1 &&
             !service.next_render_deadline(now + 103ms).has_value(),
         "a recovered static provider disarms its retry and polling deadlines");
  service.quiesce();
  expect(!service.next_render_deadline(now + 103ms).has_value() &&
             !backend.retained.empty() && backend.retained.back().empty(),
         "quiesce releases surfaces and disarms every deadline");

  const auto make_flash_deadline = [&](const double flash_hz,
                                       const layerc::core::Milliseconds expected,
                                       const std::string_view label) {
    layerc::test::TempDirectory flash_temp{"layerc-typed-flash"};
    layerc::platform::AutomationPersistence flash_persistence;
    Panel flash_panel;
    flash_panel.id = "main";
    flash_panel.name = "Main";
    flash_panel.style = layerc::core::PanelStyle::card;
    OverlayItem indicator;
    indicator.id = "indicator";
    indicator.label = "Power";
    indicator.content = layerc::core::IndicatorItem{"flag", flash_hz};
    flash_panel.items = std::vector<OverlayItem>{indicator};
    auto flash_base = game_profile("flash", flash_panel);
    auto defaults = counter_defaults(flash_base.id, 1);
    defaults.variables.front().id = "flag";
    defaults.variables.front().type = AutomationValueType::boolean;
    defaults.variables.front().default_value = AutomationValue(true);
    layerc::daemon::ConfiguredProfile flash_bundle;
    flash_bundle.source = {.path = flash_temp.path() / "flash.json",
                           .profile = flash_base};
    flash_bundle.presentation_defaults =
        layerc::core::automation_default_snapshot(defaults);
    std::vector<layerc::daemon::InputSession> flash_sessions;
    flash_sessions.emplace_back(flash_bundle, 42, flash_persistence);
    flash_sessions.back().active = true;
    const auto phase_start = layerc::core::MonotonicClock::now();
    flash_sessions.back().presentation_epoch = phase_start;
    CaptureBackend flash_backend;
    layerc::daemon::OverlayService flash_service;
    start_service(flash_service, flash_backend, flash_sessions, scan);
    flash_service.render(flash_sessions, phase_start);
    const auto deadline = flash_service.next_render_deadline(phase_start);
    expect(deadline.has_value() && *deadline == phase_start + expected,
           label);
  };
  make_flash_deadline(0.5, 1000ms,
                      "0.5 Hz indicators schedule their exact 1000 ms edge");
  make_flash_deadline(3.0, 167ms,
                      "3 Hz indicators schedule their exact 167 ms edge");
  make_flash_deadline(4.0, 167ms,
                      "4 Hz indicators are capped at the 3 Hz 167 ms edge");

  layerc::test::TempDirectory live_temp{"layerc-typed-overlay-live"};
  layerc::platform::AutomationPersistence live_persistence;
  Panel live_panel = typed_profile_panel();
  live_panel.items = std::vector<OverlayItem>{};
  OverlayItem count;
  count.id = "count";
  count.label = "Count";
  count.content = layerc::core::CounterItem{"count", 10.0,
                                            layerc::core::CounterDisplay::number};
  OverlayItem pulse;
  pulse.id = "pulse";
  pulse.label = "Pulse";
  pulse.content = layerc::core::AutomationTimerItem{"pulse"};
  OverlayItem active;
  active.id = "active";
  active.label = "Active";
  active.content = layerc::core::CooldownActiveTimerItem{"dash"};
  live_panel.items->push_back(count);
  live_panel.items->push_back(pulse);
  live_panel.items->push_back(active);
  auto live_base = game_profile("live", live_panel);
  live_base.cooldowns.push_back({.id = "dash",
                                 .name = "Dash",
                                 .duration = 60s,
                                 .active_duration = 5s});
  auto automation = live_automation(live_base.id);
  layerc::daemon::ConfiguredProfile live_bundle;
  live_bundle.source = {.path = live_temp.path() / "live.json",
                        .profile = live_base};
  auto &automation_sidecar =
      live_bundle.modules.get<layerc::core::AutomationModule>();
  automation_sidecar.profile = automation;
  automation_sidecar.state = layerc::platform::ModuleSidecarState::valid;
  live_bundle.presentation_defaults =
      layerc::core::automation_default_snapshot(automation);
  std::vector<layerc::daemon::InputSession> live_sessions;
  live_sessions.emplace_back(live_bundle, 42, live_persistence);
  live_sessions.back().active = true;
  const auto live_now = layerc::core::MonotonicClock::now();
  live_sessions.back().presentation_epoch = live_now;
  (void)live_sessions.back().automation.cooldowns().trigger("dash", live_now);
  CaptureBackend live_backend;
  layerc::daemon::OverlayService live_service;
  start_service(live_service, live_backend, live_sessions, scan);
  live_service.render(live_sessions, live_now);
  expect(live_sessions.front().automation.runtime() != nullptr,
         "enabled automation uses its live runtime snapshot");
  const auto *live_count = find_item(live_backend.frames.back().frame, "count");
  const auto *live_pulse = find_item(live_backend.frames.back().frame, "pulse");
  const auto *live_active = find_item(live_backend.frames.back().frame, "active");
  expect(live_count != nullptr && live_count->label == "Count  7/10" &&
             live_pulse != nullptr && live_pulse->active &&
             live_active != nullptr && live_active->active,
         "live variable, automation timer and cooldown active timer reach frames");
  const auto active_deadline = live_service.next_render_deadline(live_now);
  expect(active_deadline.has_value() &&
             *active_deadline == live_now + layerc::daemon::overlay_repaint_interval,
         "active timers repaint at min(expiry, now plus 100 ms)");

  const auto press_at = live_now + 10ms;
  expect(live_sessions.front().automation.process(
             input_event(AutomationEventKind::input_pressed, press_at,
                         "keyboard:1")),
         "item-scoped override actions commit to the live snapshot");
  live_service.render(live_sessions, press_at);
  const auto &overridden_frame = live_backend.frames.back().frame;
  const auto *overridden_count = find_item(overridden_frame, "count");
  expect(overridden_count != nullptr && overridden_count->label == "Boosted" &&
             overridden_count->color == 0U &&
             find_item(overridden_frame, "pulse") == nullptr,
         "visible, text and black-color item overrides reach the immutable frame");

  const auto expired_at = press_at + 600ms;
  live_sessions.front().poll_automation(expired_at);
  live_service.render(live_sessions, expired_at);
  const auto &expired_frame = live_backend.frames.back().frame;
  const auto *expired_count = find_item(expired_frame, "count");
  expect(expired_count != nullptr && expired_count->label == "Count  7/10" &&
             expired_count->color == live_panel.bar.fill_color &&
             find_item(expired_frame, "pulse") != nullptr,
         "item visibility, text and color overrides expire to their base values");

  const auto clear_press_at = expired_at + 1ms;
  (void)live_sessions.front().automation.process(
      input_event(AutomationEventKind::input_pressed, clear_press_at,
                  "keyboard:1"));
  const auto clear_at = clear_press_at + 1ms;
  (void)live_sessions.front().automation.process(
      input_event(AutomationEventKind::input_released, clear_at,
                  "keyboard:1"));
  live_service.render(live_sessions, clear_at);
  const auto &cleared_frame = live_backend.frames.back().frame;
  const auto *cleared_count = find_item(cleared_frame, "count");
  expect(cleared_count != nullptr && cleared_count->label == "Count  7/10" &&
             cleared_count->color == live_panel.bar.fill_color &&
             find_item(cleared_frame, "pulse") != nullptr,
         "clearing the item override layer restores the base frame");
}

void registry_epoch_contracts() {
  using layerc::test::expect;
  layerc::test::TempDirectory temp{"layerc-presentation-epoch"};
  GameProfile profile;
  profile.id = "epoch-profile";
  profile.name = "Epoch Profile";
  profile.game.executable_name = "fixture";
  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  profile.panels.push_back(panel);
  const auto path = temp.path() / "profile.json";
  const layerc::platform::StoredProfile initial{.path = path,
                                                .profile = profile};
  layerc::daemon::SessionRegistry registry;
  registry.configure(std::span{&initial, 1});
  const auto scan = scan_for(42);
  const auto first_now = TimePoint{} + 10s;
  registry.refresh(scan, first_now);
  expect(registry.session_count() == 1 &&
             registry.sessions().front().presentation_epoch == first_now,
         "a newly admitted registry session uses its supplied monotonic epoch");

  profile.name = "Reloaded Profile";
  const layerc::platform::StoredProfile replacement{.path = path,
                                                     .profile = profile};
  registry.configure(std::span{&replacement, 1});
  const auto second_now = first_now + 5s;
  registry.refresh(scan, second_now);
  expect(registry.session_count() == 1 &&
             registry.sessions().front().presentation_epoch == first_now,
         "same-source and same-window replacement preserves the presentation epoch");

  auto moved_scan = scan_for(99);
  auto old_window = scan_for(42).windows.front();
  old_window.active = false;
  moved_scan.windows.push_back(std::move(old_window));
  const auto third_now = second_now + 5s;
  registry.refresh(moved_scan, third_now);
  expect(registry.session_count() == 1 &&
             registry.sessions().front().window == 99 &&
             registry.sessions().front().presentation_epoch == third_now,
         "a different target receives a new injected presentation epoch");
}

[[nodiscard]] unsigned long allocate_pixel(Display *raw,
                                           const std::uint32_t rgb) {
  XColor color{};
  color.red = static_cast<unsigned short>(((rgb >> 16U) & 0xFFU) * 257U);
  color.green = static_cast<unsigned short>(((rgb >> 8U) & 0xFFU) * 257U);
  color.blue = static_cast<unsigned short>((rgb & 0xFFU) * 257U);
  color.flags = DoRed | DoGreen | DoBlue;
  if (XAllocColor(raw, DefaultColormap(raw, DefaultScreen(raw)), &color) == 0)
    return BlackPixel(raw, DefaultScreen(raw));
  return color.pixel;
}

[[nodiscard]] OverlayFrame x11_pixel_frame() {
  auto timer = row("timer", "Timer  2.5s", OverlayItemKind::timer,
                   0xE56B44U);
  timer.remaining = 0.5;
  timer.ready = false;
  timer.active = true;
  auto counter = row("counter", "Count  2/4", OverlayItemKind::counter, 0U);
  counter.has_progress = false;
  counter.active = true;
  counter.ready = false;
  counter.pips = 4;
  counter.filled_pips = 2;
  auto indicator = row("indicator", "Power", OverlayItemKind::indicator,
                       0x39B86BU);
  indicator.has_progress = false;
  indicator.active = true;
  indicator.ready = false;
  indicator.flash_on = true;
  auto frame = paint_frame({timer, counter, indicator},
                           layerc::core::PanelStyle::bar, 360, 48);
  frame.target = {20, 20, 800, 600};
  frame.panel.bar.show_text = true;
  return frame;
}

void check_typed_x11_pixels() {
  using layerc::test::expect;
  std::string error;
  auto display = layerc::platform::X11Display::open(error);
  if (!expect(display.has_value(), "X11 display opens for typed paint")) return;
  auto *raw = reinterpret_cast<Display *>(display->raw());
  auto surface = layerc::platform::X11OverlayWindow::create(*display, error);
  if (!expect(surface.has_value(), "typed X11 surface has verified Shape input"))
    return;

  auto frame = x11_pixel_frame();
  const auto first_list =
      layerc::platform::build_overlay_display_list(frame);
  const auto timer_fill = find_op(first_list, OverlayOpKind::fill_rect, 0xE56B44U);
  expect(timer_fill != nullptr, "active typed timer has a color-resolved fill op");
  const auto geometry = layerc::core::panel_geometry(frame.panel, frame.bars.size());
  const auto pip_layout = layerc::core::item_pip_layout(
      frame.panel.style,
      layerc::core::bar_geometry(frame.panel, 1, frame.bars.size(), 1.0).text,
      4);
  expect(pip_layout.pips.size() == 4,
         "the X11 counter frame resolves four pips");
  const auto xid = surface->window();
  expect(surface->update(frame), "typed timer/counter/indicator frame paints");
  expect(surface->window() == xid && surface->input_rectangle_count() == 0,
         "typed updates preserve XID and the empty input Shape");

  XSync(raw, False);
  XImage *image = XGetImage(raw, static_cast<::Window>(xid), 0, 0,
                            static_cast<unsigned int>(first_list.bounds.width),
                            static_cast<unsigned int>(first_list.bounds.height),
                            AllPlanes, ZPixmap);
  expect(image != nullptr, "typed paint pixels can be read back");
  if (image != nullptr) {
    const auto timer_pixel = allocate_pixel(raw, 0xE56B44U);
    const auto background_pixel = allocate_pixel(raw, frame.panel.bar.background_color);
    const int timer_sample_x = timer_fill != nullptr
        ? timer_fill->rect.x + std::max(1, timer_fill->rect.width / 4) : 0;
    const int timer_sample_y = timer_fill != nullptr
        ? timer_fill->rect.y + timer_fill->rect.height / 2 : 0;
    expect(timer_fill != nullptr &&
               XGetPixel(image, timer_sample_x, timer_sample_y) == timer_pixel,
           "active timer fill pixels use its resolved color");
    const auto &timer_outline = geometry.items.front();
    const int empty_x = timer_outline.right - 4;
    const int empty_y = timer_outline.top + timer_outline.height() / 2;
    expect(XGetPixel(image, empty_x, empty_y) == background_pixel,
           "the unfilled timer track remains its panel background");
    if (pip_layout.pips.size() == 4) {
      const auto &filled = pip_layout.pips.front();
      const auto &empty = pip_layout.pips[2];
      expect(XGetPixel(image, filled.left + filled.width() / 2,
                       filled.top + filled.height() / 2) ==
                 BlackPixel(raw, DefaultScreen(raw)),
             "a filled black counter pip resolves to black pixels");
      expect(XGetPixel(image, empty.left, empty.top) ==
                     BlackPixel(raw, DefaultScreen(raw)) &&
                 XGetPixel(image, empty.left + empty.width() / 2,
                           empty.top + empty.height() / 2) == background_pixel,
             "an empty counter pip is a black outline, not a filled value");
    }
    const auto &indicator_rect = geometry.items[2];
    const int indicator_x = indicator_rect.left + 5;
    const int indicator_y = indicator_rect.top + indicator_rect.height() / 2;
    const auto indicator_pixel = allocate_pixel(raw, 0x39B86BU);
    expect(XGetPixel(image, indicator_x, indicator_y) == indicator_pixel,
           "active indicator pixels use the phase accent");
    XDestroyImage(image);
  }

  auto changed = frame;
  auto &timer = *changed.bars[0].presentation;
  timer.remaining = 0.25;
  timer.label = "Timer  1.2s";
  auto &counter = *changed.bars[1].presentation;
  counter.label = "Count  3/4";
  counter.filled_pips = 3;
  auto &indicator = *changed.bars[2].presentation;
  indicator.flash_on = false;
  const auto changed_list =
      layerc::platform::build_overlay_display_list(changed);
  expect(std::ranges::any_of(changed_list.ops, [](const OverlayOp &op) {
           return op.kind == OverlayOpKind::text && op.text == "Timer  1.2s";
         }) && std::ranges::any_of(changed_list.ops, [](const OverlayOp &op) {
           return op.kind == OverlayOpKind::text && op.text == "Count  3/4";
         }),
         "updated typed labels come from the resolved presentation");
  expect(changed_list.bounds == first_list.bounds &&
             std::ranges::any_of(changed_list.ops, [](const OverlayOp &op) {
               return op.kind == OverlayOpKind::text && op.text == "Power";
             }),
         "indicator phase updates preserve bounds and label");
  expect(!has_op(changed_list, OverlayOpKind::fill_rect, 0x39B86BU),
         "the indicator off phase removes only its accent fill");
  expect(surface->update(changed) && surface->window() == xid &&
             surface->input_rectangle_count() == 0,
         "value and phase updates retain the same click-through X11 window");
  XSync(raw, False);
  image = XGetImage(raw, static_cast<::Window>(xid), 0, 0,
                    static_cast<unsigned int>(changed_list.bounds.width),
                    static_cast<unsigned int>(changed_list.bounds.height),
                    AllPlanes, ZPixmap);
  expect(image != nullptr, "updated typed paint pixels can be read back");
  if (image != nullptr) {
    const int shorter_fill_x = timer_fill != nullptr
        ? timer_fill->rect.x + 120 : 0;
    const int shorter_fill_y = timer_fill != nullptr
        ? timer_fill->rect.y + timer_fill->rect.height / 2 : 0;
    const auto background_pixel = allocate_pixel(raw, changed.panel.bar.background_color);
    expect(timer_fill != nullptr &&
               XGetPixel(image, shorter_fill_x, shorter_fill_y) == background_pixel,
           "a timer value update shrinks its painted progress region");
    const auto &third_pip = pip_layout.pips[2];
    expect(XGetPixel(image, third_pip.left + third_pip.width() / 2,
                     third_pip.top + third_pip.height() / 2) ==
               BlackPixel(raw, DefaultScreen(raw)),
           "a counter value update fills the next pip");
    const auto &indicator_rect = geometry.items[2];
    expect(XGetPixel(image, indicator_rect.left + 5,
                     indicator_rect.top + indicator_rect.height() / 2) ==
               background_pixel,
           "an indicator off phase leaves the accent area unfilled");
    XDestroyImage(image);
  }
}

void check_exposure_dispatch(const bool with_input) {
  using layerc::test::expect;
  std::string error;
  auto display = layerc::platform::X11Display::open(error);
  if (!expect(display.has_value(), "X11 provider display opens for exposure")) return;
  layerc::daemon::X11Provider provider(std::move(*display));
  if (with_input &&
      !expect(provider.open_input(error), "XI2 passive input opens for exposure path"))
    return;
  auto *backend = static_cast<layerc::daemon::X11OverlayBackend *>(
      provider.overlay());
  if (!expect(backend != nullptr, "X11 provider exposes its overlay backend")) return;

  auto frame = paint_frame({[] {
    auto item = row("count", "Count  2/4", OverlayItemKind::counter, 0U);
    item.has_progress = false;
    item.active = true;
    item.ready = false;
    item.pips = 4;
    item.filled_pips = 2;
    return item;
  }()}, layerc::core::PanelStyle::bar, 260, 48);
  frame.panel.bar.background_color = 0x272D35U;
  frame.target = {10, 10, 500, 400};
  const std::string key = with_input ? "static-input" : "static-closed";
  expect(backend->show(key, frame), "static typed provider overlay is shown");
  auto windows = backend->windows();
  if (!expect(windows.size() == 1, "one typed overlay window is owned")) return;
  const auto xid = static_cast<::Window>(windows.front());
  auto *raw = reinterpret_cast<Display *>(provider.display().raw());

  for (int pass = 0; pass < 8 && provider.has_pending_events(); ++pass)
    (void)provider.dispatch_pending();
  provider.tracker().ignore_windows({static_cast<std::uint64_t>(xid)});
  XEvent synthetic{};
  synthetic.type = Expose;
  synthetic.xexpose.window = xid;
  expect(!provider.tracker().observe(synthetic) &&
             provider.tracker().take_overlay_exposed() &&
             !provider.tracker().take_overlay_exposed(),
         "an ignored overlay Expose requests repaint once without discovery");
  synthetic.xexpose.window = DefaultRootWindow(raw);
  expect(!provider.tracker().observe(synthetic) &&
             !provider.tracker().take_overlay_exposed(),
         "a foreign Expose never requests overlay repaint");

  XWindowAttributes attributes{};
  expect(XGetWindowAttributes(raw, xid, &attributes) != 0,
         "the static overlay attributes are queryable");
  XClearArea(raw, xid, 0, 0, static_cast<unsigned int>(attributes.width),
             static_cast<unsigned int>(attributes.height), True);
  XSync(raw, False);
  const auto exposed = provider.dispatch_pending();
  expect(exposed.repaint_requested && exposed.events.empty() &&
             !exposed.windows_changed && !exposed.devices_changed,
         with_input
             ? "input-open dispatch carries only a repaint request"
             : "input-closed dispatch carries only a repaint request");
  const auto drained = provider.dispatch_pending();
  expect(!drained.repaint_requested && drained.events.empty() &&
             !drained.windows_changed && !drained.devices_changed,
         "a second drained exposure pass has no repaint or other work");

  expect(backend->show(key, frame), "the same static frame repaints after exposure");
  windows = backend->windows();
  expect(windows.size() == 1 && windows.front() == xid,
         "exposure repaint preserves the owned window XID");
  int shape_count = -1;
  int shape_order = 0;
  XRectangle *shapes = XShapeGetRectangles(raw, xid, ShapeInput,
                                           &shape_count, &shape_order);
  if (shapes != nullptr) XFree(shapes);
  expect(shape_count == 0,
         "exposure repaint preserves the verified empty input Shape");

  const auto list = layerc::platform::build_overlay_display_list(frame);
  const auto area = layerc::core::bar_geometry(frame.panel, 0, 1, 1.0).text;
  const auto pips = layerc::core::item_pip_layout(frame.panel.style, area, 4);
  XSync(raw, False);
  XImage *image = XGetImage(raw, xid, 0, 0,
                            static_cast<unsigned int>(list.bounds.width),
                            static_cast<unsigned int>(list.bounds.height),
                            AllPlanes, ZPixmap);
  expect(image != nullptr, "exposure repaint pixels can be read back");
  if (image != nullptr && pips.pips.size() == 4) {
    expect(XGetPixel(image, pips.pips.front().left + pips.pips.front().width() / 2,
                     pips.pips.front().top + pips.pips.front().height() / 2) ==
               BlackPixel(raw, DefaultScreen(raw)),
           "exposure repaint restores the static typed pip pixels");
    XDestroyImage(image);
  } else if (image != nullptr) {
    XDestroyImage(image);
  }
}

} // namespace

void linux_typed_paint_contracts() { paint_contracts(); }
void linux_typed_service_contracts() {
  service_contracts();
  registry_epoch_contracts();
}
void linux_typed_x11_pixels() {
  layerc::test::require_x11();
  check_typed_x11_pixels();
  check_exposure_dispatch(false);
  check_exposure_dispatch(true);
}
