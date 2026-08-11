#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace layerc::core {

using MonotonicClock = std::chrono::steady_clock;
using TimePoint = MonotonicClock::time_point;
using Milliseconds = std::chrono::milliseconds;

enum class InputDevice : std::uint8_t {
    keyboard,
    mouse,
    controller,
};

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

enum class RetriggerPolicy : std::uint8_t {
    ignore,
    restart,
};

struct CooldownDefinition {
    std::string id;
    std::string name;
    Milliseconds duration{};
    std::uint32_t max_charges{1};
    RetriggerPolicy retrigger{RetriggerPolicy::ignore};
    bool stack_charges{};

    bool operator==(const CooldownDefinition&) const = default;
};

struct CooldownState {
    std::string id;
    std::uint32_t available_charges{};
    std::uint32_t max_charges{};
    std::optional<TimePoint> next_charge_at;
};

struct InputBinding {
    std::string cooldown_id;
    InputChord chord;

    bool operator==(const InputBinding&) const = default;
};

enum class PanelLayout : std::uint8_t {
    row,
    column,
    grid,
};

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

    bool operator==(const GameMatch&) const = default;
};

struct GameProfile {
    std::uint32_t schema_version{2};
    std::string id;
    std::string name;
    // A disabled profile stays on disk and remains editable, but LayerD does
    // not create a runtime instance for it. Keep the default true so JSON
    // profiles written before this field existed remain active.
    bool enabled{true};
    GameMatch game;
    std::vector<Panel> panels;
    std::vector<CooldownDefinition> cooldowns;
    std::vector<InputBinding> bindings;

    bool operator==(const GameProfile&) const = default;
};

}  // namespace layerc::core
