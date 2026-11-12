#include "layerc/core/panel_presentation.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <type_traits>
#include <variant>

namespace layerc::core {
namespace {

// "name  value", without a dangling separator when either side is empty.
std::string join_label(const std::string &name, const std::string_view value) {
  if (name.empty()) return std::string(value);
  if (value.empty()) return name;
  std::string text = name;
  text += "  ";
  text += value;
  return text;
}

double fraction(const Milliseconds part, const Milliseconds whole) noexcept {
  if (whole <= Milliseconds::zero()) return 0.0;
  return std::clamp(static_cast<double>(part.count()) /
                        static_cast<double>(whole.count()),
                    0.0, 1.0);
}

// Shortest round-trip text, independent of the process locale.
std::string number_text(const double value) {
  std::array<char, 32> buffer{};
  const auto converted =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return converted.ec == std::errc{} ? std::string(buffer.data(), converted.ptr)
                                     : std::string{};
}

struct RowSources {
  std::span<const CooldownDefinition> definitions;
  std::span<const CooldownState> states;
  const AutomationRuntimeSnapshot *automation;
  TimePoint epoch;
  TimePoint now;
  std::optional<TimePoint> next_wakeup;

  void wake_at(const TimePoint at) {
    next_wakeup = earliest_deadline(next_wakeup, at);
  }
  [[nodiscard]] const CooldownDefinition *
  definition(const std::string_view id, std::size_t &index) const {
    const auto found = std::ranges::find(definitions, id, &CooldownDefinition::id);
    if (found == definitions.end()) return nullptr;
    index = static_cast<std::size_t>(found - definitions.begin());
    return &*found;
  }
  [[nodiscard]] const CooldownState *state(const std::size_t index,
                                           const std::string_view id) const {
    return cooldown_state_at(states, index, id);
  }
  [[nodiscard]] const AutomationValue *variable(const std::string &id) const {
    const auto found = automation->variables.find(id);
    return found == automation->variables.end() ? nullptr : &found->second;
  }
};

void make_unavailable(ItemPresentation &row, const ItemUnavailableReason reason) {
  row.unavailable = true;
  row.unavailable_reason = reason;
  row.value_text = "Unavailable";
  row.label = join_label(row.name, row.value_text);
  row.active = false;
  row.ready = true;
  row.has_progress = false;
  row.remaining = 0.0;
  row.remaining_time = {};
  row.available_charges = 0;
  row.max_charges = 1;
  row.pips = 0;
  row.filled_pips = 0;
}

// Exactly the pre-items cooldown row; legacy rows keep the status text bytes.
void present_cooldown(ItemPresentation &row, const CooldownDefinition &definition,
                      const CooldownState *state, const TimePoint now,
                      const bool legacy) {
  row.cooldown_id = definition.id;
  row.kind = OverlayItemKind::cooldown;
  row.available_charges = definition.max_charges;
  row.max_charges = definition.max_charges;
  if (state != nullptr) {
    row.remaining = cooldown_progress(*state, now);
    row.remaining_time = cooldown_remaining_ms(state->next_charge_at, now);
    row.available_charges = state->available_charges;
    row.max_charges = state->max_charges;
    row.ready = cooldown_ready(*state);
    row.value_text = cooldown_status_value(*state, now);
  }
  if (legacy)
    row.label = state != nullptr ? cooldown_status_text(definition, *state, now)
                                 : definition.name;
  else
    row.label = join_label(row.name, row.value_text);
  row.active = !row.ready;
}

void present_countdown(ItemPresentation &row, RowSources &sources,
                       const std::optional<TimePoint> due,
                       const Milliseconds length) {
  row.kind = OverlayItemKind::timer;
  row.active = due.has_value() && *due > sources.now;
  row.ready = !row.active;
  row.remaining = 0.0;
  if (row.active) {
    row.remaining_time = cooldown_remaining_ms(due, sources.now);
    row.remaining = fraction(row.remaining_time, length);
    row.value_text = format_tenths(row.remaining_time);
    sources.wake_at(*due);
  }
  row.available_charges = 0;
  row.max_charges = 1;
  row.label = join_label(row.name, row.value_text);
}

void present_active_timer(ItemPresentation &row, RowSources &sources,
                          const CooldownActiveTimerItem &item) {
  row.kind = OverlayItemKind::timer;
  row.cooldown_id = item.cooldown_id;
  std::size_t index{};
  const auto *definition = sources.definition(item.cooldown_id, index);
  if (definition == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_source);
  if (definition->active_duration <= Milliseconds::zero())
    return make_unavailable(row, ItemUnavailableReason::disabled_source);
  const auto *state = sources.state(index, item.cooldown_id);
  const auto length = state != nullptr && state->active_duration > Milliseconds::zero()
                          ? state->active_duration
                          : definition->active_duration;
  present_countdown(row, sources,
                    state != nullptr ? state->active_until : std::nullopt, length);
}

void present_automation_timer(ItemPresentation &row, RowSources &sources,
                              const AutomationTimerItem &item) {
  row.kind = OverlayItemKind::timer;
  if (sources.automation == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_automation);
  // Timer IDs are session-wide. An absent running instance is stopped only
  // when its ID is declared; an undeclared/removed ID is a missing source.
  if (std::ranges::find(sources.automation->declared_timer_ids, item.timer_id) ==
      sources.automation->declared_timer_ids.end())
    return make_unavailable(row, ItemUnavailableReason::missing_source);
  const auto &timers = sources.automation->timers;
  const auto found = std::ranges::find(timers, item.timer_id, &AutomationTimer::id);
  if (found == timers.end())
    return present_countdown(row, sources, std::nullopt, Milliseconds::zero());
  present_countdown(row, sources, found->due, found->interval);
}

void present_counter(ItemPresentation &row, RowSources &sources,
                     const CounterItem &item) {
  row.kind = OverlayItemKind::counter;
  if (sources.automation == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_automation);
  const auto *value = sources.variable(item.variable);
  if (value == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_source);
  const bool pips = item.display == CounterDisplay::pips;
  std::string text;
  double numeric{};
  bool nonzero{};
  std::optional<std::int64_t> integer;
  if (const auto *whole = std::get_if<std::int64_t>(&value->storage)) {
    integer = *whole;
    text = std::to_string(*whole);
    numeric = static_cast<double>(*whole);
    nonzero = *whole != 0;
  } else if (const auto *real = std::get_if<double>(&value->storage);
             real != nullptr && std::isfinite(*real) && !pips) {
    text = number_text(*real);
    numeric = *real;
    nonzero = *real != 0.0;
  } else {
    return make_unavailable(row, ItemUnavailableReason::wrong_type);
  }
  if (pips && (!item.max || *item.max < 1.0 ||
               *item.max > static_cast<double>(max_counter_pips) ||
               std::floor(*item.max) != *item.max))
    return make_unavailable(row, ItemUnavailableReason::wrong_type);

  row.value_text = text;
  row.has_progress = item.max.has_value();
  row.remaining = 0.0;
  if (item.max) {
    const auto max_text = pips ? std::to_string(static_cast<std::int64_t>(*item.max))
                               : number_text(*item.max);
    row.value_text += "/" + max_text;
    row.remaining = std::clamp(numeric / *item.max, 0.0, 1.0);
  }
  if (pips) {
    const auto count = static_cast<std::int64_t>(*item.max);
    row.pips = static_cast<std::uint32_t>(count);
    row.filled_pips = static_cast<std::uint32_t>(std::clamp<std::int64_t>(*integer, 0, count));
  }
  row.active = nonzero;
  row.ready = !row.active;
  row.available_charges = 0;
  row.max_charges = 1;
  row.label = join_label(row.name, row.value_text);
}

void present_indicator(ItemPresentation &row, RowSources &sources,
                       const IndicatorItem &item) {
  row.kind = OverlayItemKind::indicator;
  if (sources.automation == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_automation);
  const auto *value = sources.variable(item.variable);
  if (value == nullptr)
    return make_unavailable(row, ItemUnavailableReason::missing_source);
  if (const auto *flag = std::get_if<bool>(&value->storage))
    row.active = *flag;
  else if (const auto *whole = std::get_if<std::int64_t>(&value->storage))
    row.active = *whole != 0;
  else if (const auto *real = std::get_if<double>(&value->storage))
    row.active = std::isfinite(*real) && *real != 0.0;
  else
    return make_unavailable(row, ItemUnavailableReason::wrong_type);

  row.has_progress = false;
  row.remaining = 0.0;
  row.ready = !row.active;
  row.available_charges = 0;
  row.max_charges = 1;
  row.label = row.name;
  // Every renderer draws this phase, so the cap holds on every surface.
  const double hz = indicator_flash_hz(item.flash_hz);
  if (!row.active || hz == 0.0)
    return;
  // 50% duty from the presentation epoch; phase zero is ON.
  // Upward, so no stored rate can shorten the period below the 3 Hz cap.
  const auto period =
      std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(1000.0 / hz)));
  const auto elapsed =
      sources.now > sources.epoch
          ? std::chrono::duration_cast<Milliseconds>(sources.now - sources.epoch).count()
          : std::int64_t{0};
  const auto phase = elapsed % period;
  row.flash_on = 2 * phase < period;
  const auto edge = elapsed - phase + (row.flash_on ? (period + 1) / 2 : period);
  sources.wake_at(sources.epoch + Milliseconds(edge));
}

[[nodiscard]] const ItemPresentationOverride *
item_override(const PanelPresentationOverrides &overrides,
              const std::string_view item_id) {
  const auto found = std::ranges::find(overrides.items, item_id,
                                       &ItemPresentationOverride::item_id);
  return found == overrides.items.end() ? nullptr : &*found;
}

[[nodiscard]] std::string source_name(const OverlayItem &item,
                                      const RowSources &sources) {
  if (item.label) return *item.label;
  return std::visit(
      [&](const auto &content) -> std::string {
        using Content = std::decay_t<decltype(content)>;
        if constexpr (std::is_same_v<Content, CooldownItem> ||
                      std::is_same_v<Content, CooldownActiveTimerItem>) {
          std::size_t index{};
          const auto *definition = sources.definition(content.cooldown_id, index);
          return definition != nullptr ? definition->name : content.cooldown_id;
        } else if constexpr (std::is_same_v<Content, AutomationTimerItem>) {
          return content.timer_id;
        } else if constexpr (std::is_same_v<Content, CounterItem> ||
                             std::is_same_v<Content, IndicatorItem>) {
          return content.variable;
        } else {
          return {};
        }
      },
      item.content);
}

[[nodiscard]] bool needs_attention(const ItemPresentation &row) noexcept {
  if (row.unavailable) return true;
  return row.kind == OverlayItemKind::cooldown ? !row.ready : row.active;
}

// Shared by legacy and typed rows: override text and color, then visibility.
void finish_row(ItemPresentation &row, const Panel &panel,
                const PanelPresentationOverrides &overrides,
                const ItemPresentationOverride *item) {
  row.color = item != nullptr && item->color ? *item->color : panel.bar.fill_color;
  if (item != nullptr && item->text)
    row.label = *item->text;
  else if (overrides.text)
    row.label = *overrides.text;
}

} // namespace

std::string legacy_item_id(const std::size_t index) {
  return std::string(legacy_item_id_prefix) + std::to_string(index);
}

std::optional<std::size_t>
parse_legacy_item_index(const std::string_view item_id) noexcept {
  if (!item_id.starts_with(legacy_item_id_prefix)) return std::nullopt;
  const auto digits = item_id.substr(legacy_item_id_prefix.size());
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
    return std::nullopt;
  std::size_t index{};
  const auto *end = digits.data() + digits.size();
  const auto [last, error] = std::from_chars(digits.data(), end, index);
  if (error != std::errc{} || last != end) return std::nullopt;
  return index;
}

bool panel_item_targetable(const Panel &panel,
                           const std::string_view item_id) noexcept {
  if (panel.items)
    return std::ranges::any_of(*panel.items, [&](const OverlayItem &item) {
      return item.id == item_id &&
             !std::holds_alternative<OpaqueOverlayItem>(item.content);
    });
  const auto index = parse_legacy_item_index(item_id);
  return index.has_value() && *index < panel.cooldown_ids.size();
}

PanelPresentation present_panel(const Panel &panel,
                                const std::span<const CooldownDefinition> definitions,
                                const std::span<const CooldownState> states,
                                const AutomationRuntimeSnapshot *const automation,
                                const PanelPresentationOverrides &overrides,
                                const TimePoint now) {
  PanelPresentation result;
  RowSources sources{.definitions = definitions,
                     .states = states,
                     .automation = automation,
                     .epoch = overrides.presentation_epoch,
                     .now = now,
                     .next_wakeup = std::nullopt};
  bool attention{};
  const auto keep = [&](ItemPresentation row, const ItemPresentationOverride *item) {
    finish_row(row, panel, overrides, item);
    attention = attention || needs_attention(row);
    result.items.push_back(std::move(row));
  };

  if (!panel.items) {
    result.items.reserve(panel.cooldown_ids.size());
    for (std::size_t index = 0; index < panel.cooldown_ids.size(); ++index) {
      const auto &cooldown_id = panel.cooldown_ids[index];
      std::size_t position{};
      const auto *definition = sources.definition(cooldown_id, position);
      if (definition == nullptr) continue;
      ItemPresentation row;
      row.item_id = legacy_item_id(index);
      const auto *item = item_override(overrides, row.item_id);
      if (item != nullptr && item->visible == false) continue;
      row.name = definition->name;
      present_cooldown(row, *definition, sources.state(position, cooldown_id),
                       now, true);
      keep(std::move(row), item);
    }
  } else {
    result.items.reserve(panel.items->size());
    for (const auto &source : *panel.items) {
      // Opaque entries are preserved data, never rows.
      if (std::holds_alternative<OpaqueOverlayItem>(source.content)) continue;
      const auto *item = item_override(overrides, source.id);
      if (item != nullptr && item->visible == false) continue;
      ItemPresentation row;
      row.item_id = source.id;
      row.name = source_name(source, sources);
      if (const auto *cooldown = std::get_if<CooldownItem>(&source.content)) {
        std::size_t position{};
        const auto *definition = sources.definition(cooldown->cooldown_id, position);
        row.kind = OverlayItemKind::cooldown;
        row.cooldown_id = cooldown->cooldown_id;
        if (definition == nullptr)
          make_unavailable(row, ItemUnavailableReason::missing_source);
        else
          present_cooldown(row, *definition,
                           sources.state(position, cooldown->cooldown_id), now,
                           false);
      } else if (const auto *active =
                     std::get_if<CooldownActiveTimerItem>(&source.content)) {
        present_active_timer(row, sources, *active);
      } else if (const auto *timer =
                     std::get_if<AutomationTimerItem>(&source.content)) {
        present_automation_timer(row, sources, *timer);
      } else if (const auto *counter = std::get_if<CounterItem>(&source.content)) {
        present_counter(row, sources, *counter);
      } else if (const auto *indicator =
                     std::get_if<IndicatorItem>(&source.content)) {
        present_indicator(row, sources, *indicator);
      }
      keep(std::move(row), item);
    }
  }

  result.next_wakeup = sources.next_wakeup;
  if (overrides.visible.has_value())
    result.visible = *overrides.visible;
  else
    result.visible = overrides.edit_mode || attention ||
                     panel.style != PanelStyle::bar || !panel.bar.hide_when_ready;
  return result;
}

PanelPresentation present_panel(const Panel &panel,
                                const std::span<const CooldownDefinition> definitions,
                                const std::span<const CooldownState> states,
                                const PanelPresentationOverrides &overrides,
                                const TimePoint now) {
  return present_panel(panel, definitions, states, nullptr, overrides, now);
}

std::vector<std::string> overlay_item_diagnostics(
    const GameProfile &profile, const AutomationRuntimeSnapshot *automation,
    const TimePoint now) {
  std::vector<std::string> result;
  for (const auto &panel : profile.panels) {
    if (!panel.items) continue;
    const auto prefix =
        "profile " + profile.id + ", panel " + panel.id + ", item ";
    for (const auto &item : *panel.items)
      if (std::holds_alternative<OpaqueOverlayItem>(item.content))
        result.push_back(prefix + item.id +
                         ": unsupported item kind (preserved; not rendered)");
    const auto shown =
        present_panel(panel, profile.cooldowns, {}, automation, {}, now);
    for (const auto &row : shown.items) {
      if (!row.unavailable) continue;
      std::string_view reason;
      switch (row.unavailable_reason) {
      case ItemUnavailableReason::missing_automation:
        reason = "missing automation module";
        break;
      case ItemUnavailableReason::missing_source:
        reason = "missing source";
        break;
      case ItemUnavailableReason::wrong_type:
        reason = "source has incompatible type";
        break;
      case ItemUnavailableReason::disabled_source:
        reason = "source is disabled";
        break;
      case ItemUnavailableReason::none:
        continue;
      }
      result.push_back(prefix + row.item_id + ": " + std::string(reason));
    }
  }
  return result;
}

} // namespace layerc::core
