#pragma once

#include "layerc/core/json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace layerc::core {

inline constexpr std::size_t max_typed_profile_panels = 64;
inline constexpr std::size_t max_overlay_items_per_panel = 32;
inline constexpr std::uint32_t max_counter_pips = 8;
inline constexpr double min_indicator_flash_hz = 0.5;
inline constexpr double max_indicator_flash_hz = 4.0;
// Flashing stays within WCAG 2.3.1's three flashes per second. Profiles keep
// any stored rate up to max_indicator_flash_hz; every renderer flashes at
// indicator_flash_hz's capped rate (DesignSystem.md, Readability).
inline constexpr double max_shown_indicator_flash_hz = 3.0;

// The rate an indicator flashes at: its stored rate capped at three flashes
// per second, or 0 when it does not flash.
[[nodiscard]] constexpr double indicator_flash_hz(const double stored) noexcept {
  if (!(stored > 0.0) || stored > std::numeric_limits<double>::max()) return 0.0;
  return std::clamp(stored, min_indicator_flash_hz, max_shown_indicator_flash_hz);
}

enum class OverlayItemKind : std::uint8_t { cooldown, timer, counter, indicator };
enum class OverlayTimerSource : std::uint8_t { automation, cooldown_active };
enum class CounterDisplay : std::uint8_t { number, pips };

struct CooldownItem {
  std::string cooldown_id;
  bool operator==(const CooldownItem &) const = default;
};
struct AutomationTimerItem {
  std::string timer_id;
  bool operator==(const AutomationTimerItem &) const = default;
};
struct CooldownActiveTimerItem {
  std::string cooldown_id;
  bool operator==(const CooldownActiveTimerItem &) const = default;
};
struct CounterItem {
  std::string variable;
  std::optional<double> max;
  CounterDisplay display{CounterDisplay::number};
  bool operator==(const CounterItem &) const = default;
};
struct IndicatorItem {
  std::string variable;
  double flash_hz{};
  bool operator==(const IndicatorItem &) const = default;
};
struct OpaqueOverlayItem {
  json::Value value;
  bool operator==(const OpaqueOverlayItem &) const = default;
};
using OverlayItemContent = std::variant<CooldownItem, AutomationTimerItem,
    CooldownActiveTimerItem, CounterItem, IndicatorItem, OpaqueOverlayItem>;
struct OverlayItem {
  std::string id;
  std::optional<std::string> label{};
  OverlayItemContent content{CooldownItem{}};
  json::Value::Object extra_fields{};
  bool operator==(const OverlayItem &) const = default;
};

[[nodiscard]] inline std::optional<OverlayItemKind>
overlay_item_kind(const OverlayItem &item) noexcept {
  if (std::holds_alternative<CooldownItem>(item.content))
    return OverlayItemKind::cooldown;
  if (std::holds_alternative<AutomationTimerItem>(item.content) ||
      std::holds_alternative<CooldownActiveTimerItem>(item.content))
    return OverlayItemKind::timer;
  if (std::holds_alternative<CounterItem>(item.content))
    return OverlayItemKind::counter;
  if (std::holds_alternative<IndicatorItem>(item.content))
    return OverlayItemKind::indicator;
  return std::nullopt;
}

[[nodiscard]] inline std::vector<std::string>
overlay_cooldown_projection(const std::span<const OverlayItem> items) {
  std::vector<std::string> cooldown_ids;
  cooldown_ids.reserve(items.size());
  for (const auto &item : items)
    if (const auto *cooldown = std::get_if<CooldownItem>(&item.content))
      cooldown_ids.push_back(cooldown->cooldown_id);
  return cooldown_ids;
}

} // namespace layerc::core
