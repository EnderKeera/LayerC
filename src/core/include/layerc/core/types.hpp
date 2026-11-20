#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "layerc/core/enum_wire.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/overlay_item.hpp"

namespace layerc::core {

using MonotonicClock = std::chrono::steady_clock;
using TimePoint = MonotonicClock::time_point;
using Milliseconds = std::chrono::milliseconds;

// The earlier of two optional deadlines; an absent deadline never wins.
[[nodiscard]] constexpr std::optional<TimePoint>
earliest_deadline(const std::optional<TimePoint> left,
                  const std::optional<TimePoint> right) noexcept {
  if (!left.has_value())
    return right;
  if (!right.has_value())
    return left;
  return *right < *left ? right : left;
}

// Longest cooldown or automation duration accepted by every profile surface.
inline constexpr Milliseconds max_cooldown_duration{86'400'000};
// Inclusive bounds shared by the profile parser, overlay overrides, and the
// editor fields.
inline constexpr std::uint32_t min_cooldown_charges = 1;
inline constexpr std::uint32_t max_cooldown_charges = 99;
inline constexpr std::size_t max_cooldown_groups = 64;
inline constexpr std::uint32_t min_bar_width = 40;
inline constexpr std::uint32_t max_bar_width = 1'200;
inline constexpr std::uint32_t min_bar_height = 16;
inline constexpr std::uint32_t max_bar_height = 240;

// Opaque native-window handle shared by the capture boundary and the daemon
// kernel. A leaf struct, not kernel policy, so it lives in core and the
// capture layer stays independent of the runtime.
struct WindowId {
  std::uint64_t opaque{};
  auto operator<=>(const WindowId&) const = default;
};

enum class InputDevice : std::uint8_t {
  keyboard,
  mouse,
  controller,
  generic_hid,
};

inline constexpr std::array input_device_wire{
    EnumWireEntry{InputDevice::keyboard, "keyboard"},
    EnumWireEntry{InputDevice::mouse, "mouse"},
    EnumWireEntry{InputDevice::controller, "controller"},
    EnumWireEntry{InputDevice::generic_hid, "hid"}};

[[nodiscard]] constexpr std::string_view
input_device_label(const InputDevice device) noexcept {
  return enum_wire_name(input_device_wire, device).value_or("unknown");
}

struct InputToken {
  InputDevice device{InputDevice::keyboard};
  std::uint16_t code{};
  std::uint8_t controller_index{};

  auto operator<=>(const InputToken&) const = default;
};

struct InputChord {
  std::vector<InputToken> tokens;

  bool operator==(const InputChord&) const = default;
};

struct InputEvent {
  InputToken token;
  bool pressed{};
};

// Automation input sources: "<device>:<code>" for pressed/released tokens
// (controller tokens append ":<controller index>"),
// "controller:<index>:<axis>" for analog axes and "hid:<binding number>" for
// HID controls. Rule source filters match these exactly.
[[nodiscard]] inline std::string input_source_id(const InputToken& token) {
  auto source = std::string(input_device_label(token.device)) + ":" +
                std::to_string(token.code);
  if (token.device == InputDevice::controller)
    source += ":" + std::to_string(token.controller_index);
  return source;
}

[[nodiscard]] inline std::string
controller_axis_source_id(const std::uint32_t controller_index,
                          const std::string_view axis) {
  return std::string(input_device_label(InputDevice::controller)) + ":" +
         std::to_string(controller_index) + ":" + std::string(axis);
}

[[nodiscard]] inline std::string
hid_source_id(const std::size_t binding_number) {
  return std::string(input_device_label(InputDevice::generic_hid)) + ":" +
         std::to_string(binding_number);
}

// Whether a rule source filter names a source of `device`.
[[nodiscard]] constexpr bool
source_filter_names_device(const std::string_view filter,
                           const InputDevice device) noexcept {
  const auto label = input_device_label(device);
  return filter.size() > label.size() && filter.starts_with(label) &&
         filter[label.size()] == ':';
}

enum class RetriggerPolicy : std::uint8_t {
  ignore,
  restart,
};

enum class RecoveryMode : std::uint8_t {
  single,
  sequential,
  parallel,
};

struct CooldownDefinition {
  std::string id;
  std::string name;
  Milliseconds duration{};
  std::uint32_t max_charges{1};
  RetriggerPolicy retrigger{RetriggerPolicy::ignore};
  // Schema <4 expressed charge recovery as a stackCharges boolean; the
  // parser derives this field from it at the read boundary, so runtime code
  // only ever sees recovery.
  RecoveryMode recovery{RecoveryMode::single};

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};
  Milliseconds active_duration{};

  bool operator==(const CooldownDefinition&) const = default;
};

struct CooldownState {
  std::string id;
  std::uint32_t available_charges{};
  std::uint32_t max_charges{};
  std::optional<TimePoint> next_charge_at;
  std::vector<TimePoint> recovery_deadlines;
  // Engine snapshots project the effective owner settings onto public IDs.
  // This runtime-only field is never persisted in profile JSON.
  Milliseconds recharge_duration{};
  // The public ID's own active window: armed by each accepted trigger, never
  // shared through a pool and never persisted. Absent once expired.
  std::optional<TimePoint> active_until;
  Milliseconds active_duration{};
};

enum class CooldownGroupMode : std::uint8_t {
  shared_pool,
  trigger_propagation,
};

struct CooldownGroup {
  std::string id;
  std::string name;
  CooldownGroupMode mode{CooldownGroupMode::trigger_propagation};
  std::vector<std::string> cooldown_ids;
  Milliseconds duration{};
  std::uint32_t max_charges{1};
  RecoveryMode recovery{RecoveryMode::single};
  RetriggerPolicy retrigger{RetriggerPolicy::ignore};

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};

  bool operator==(const CooldownGroup &) const = default;
};

struct InputBinding {
  std::string cooldown_id;
  InputChord chord;

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};
  // Token identity rather than array position keeps metadata on reordering.
  std::map<InputToken, json::Value::Object> token_extra_fields{};

  bool operator==(const InputBinding&) const = default;
};

enum class HidActivationMode : std::uint8_t {
  button,
  equals,
  axis_positive,
  axis_negative,
  relative_positive,
  relative_negative,
};

struct HidDeviceSelector {
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  std::uint16_t version{};
  std::uint16_t usage_page{};
  std::uint16_t usage{};
  std::string descriptor_hash;
  std::string serial;
  std::uint16_t stable_slot{1};

  bool operator==(const HidDeviceSelector&) const = default;
};

struct HidControlBinding {
  std::string id;
  HidDeviceSelector device;
  std::uint8_t report_id{};
  std::uint16_t usage_page{};
  std::uint16_t usage{};
  std::uint16_t link_collection{};
  HidActivationMode activation{HidActivationMode::button};
  std::int32_t discrete_value{};
  double threshold{0.5};
  double hysteresis{0.1};
  bool allow_system_class{};

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};
  json::Value::Object device_extra_fields{};
  json::Value::Object control_extra_fields{};

  bool operator==(const HidControlBinding&) const = default;
};

enum class PanelLayout : std::uint8_t {
  row,
  column,
  grid,
};

// Shared overlay geometry: one profile looks the same on the Windows and
// X11 backends, so the metrics have one owner here next to Panel. Values
// are authored at 96 dpi; each backend scales them per monitor.
struct OverlayMetrics {
  int padding{6};
  int spacing{6};
  int card_item_width{78};
  int card_item_height{72};
  int bar_inset{2};
  int corner_radius{8};
  int fill_radius{6};
  int bar_text_inset_h{8};
  int bar_text_inset_v{2};
  int card_text_inset{3};
};
inline constexpr OverlayMetrics overlay_metrics{};

// Shared overlay colors (0xRRGGBB) with the same single owner: card panels
// always paint the card palette, bar panels their BarAppearance colors. The
// Windows overlay window shows its backdrop behind cards and grouped bars.
struct OverlayPalette {
  std::uint32_t card_background{0x272D35};
  std::uint32_t card_border{0x50BAC8};
  std::uint32_t card_text{0xE4E8EC};
  std::uint32_t window_backdrop{0x151A20};
};
inline constexpr OverlayPalette overlay_palette{};

struct OverlayGridExtent {
  std::size_t columns{};
  std::size_t rows{};
};

// Card-grid shape shared by both backends: row lays out horizontally,
// column vertically, grid fills the squarest rectangle. A zero count yields
// an empty single row (never a divide-by-zero); callers size empty panels
// without placing items.
[[nodiscard]] inline OverlayGridExtent
overlay_grid_extent(const PanelLayout layout, const std::size_t count) noexcept {
  if (layout == PanelLayout::column) return {1, count};
  if (layout == PanelLayout::grid && count > 0) {
    const auto columns = static_cast<std::size_t>(
        std::ceil(std::sqrt(static_cast<double>(count))));
    return {columns, (count + columns - 1) / columns};
  }
  return {count, 1};
}

enum class PanelStyle : std::uint8_t {
  card,
  bar,
};

struct BarAppearance {
  std::uint32_t width{260};
  std::uint32_t height{34};
  std::uint32_t fill_color{0x50BAC8};
  std::uint32_t background_color{0x272D35};
  std::uint32_t border_color{0x50BAC8};
  std::uint32_t text_color{0xE4E8EC};
  // Profiles written before showText existed keep their labels.
  bool show_text{true};
  bool hide_when_ready{true};

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};

  bool operator==(const BarAppearance&) const = default;
};

struct Panel {
  std::string id;
  std::string name;
  PanelLayout layout{PanelLayout::row};
  PanelStyle style{PanelStyle::card};
  BarAppearance bar;
  double anchor_x{0.5};
  double anchor_y{0.5};
  std::vector<std::string> cooldown_ids;

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};
  std::optional<std::vector<OverlayItem>> items{};

  bool operator==(const Panel&) const = default;
};

struct GameMatch {
  // Development/test-only profile mode. A global profile is not attached to
  // an executable and remains active while any other application is focused.
  bool global{};
  std::string executable_path;
  std::string executable_name;
  std::string window_class;
  std::string window_title;

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};

  bool operator==(const GameMatch&) const = default;
};

enum class VisualRuntime : std::uint8_t {
  inline_process,
  isolated,
};

enum class ProfileModuleKind : std::uint8_t {
  visual,
  automation,
  companion,
};

struct ProfileModule {
  ProfileModuleKind kind{ProfileModuleKind::visual};
  std::uint32_t schema_version{2};

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};

  bool operator==(const ProfileModule&) const = default;
};

struct GameProfile {
  std::uint32_t schema_version{4};
  std::string id;
  std::string name;
  // A disabled profile stays on disk and remains editable, but LayerD does
  // not create a runtime instance for it. Keep the default true so JSON
  // profiles written before this field existed remain active.
  bool enabled{true};
  GameMatch game;
  VisualRuntime visual_runtime{VisualRuntime::inline_process};
  std::vector<ProfileModule> modules;
  std::vector<Panel> panels;
  std::vector<CooldownDefinition> cooldowns;
  std::vector<CooldownGroup> cooldown_groups;
  std::vector<HidControlBinding> hid_bindings;
  std::vector<InputBinding> bindings;

  // Opaque, value-owned JSON fields not interpreted by this profile schema.
  // Keep them with the entity through edits, history and durable writes.
  json::Value::Object extra_fields{};

  bool operator==(const GameProfile&) const = default;
};

}  // namespace layerc::core
