#pragma once

#include "layerc/core/companion.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace layerc::core {

// Build a wire-bounded immutable snapshot from a session's cooldown states.
// An empty result means even the reduced snapshot could not be serialized.
[[nodiscard]] std::string build_companion_snapshot(
    std::string_view profile_id, std::string_view session_key,
    std::span<const Panel> panels,
    std::span<const CooldownDefinition> definitions,
    std::span<const CooldownState> states,
    const std::optional<CompanionProfile> &companion = std::nullopt,
    TimePoint now = MonotonicClock::now());

} // namespace layerc::core
