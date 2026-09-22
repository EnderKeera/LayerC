#pragma once

// Shared per-session automation lifecycle policy for the Windows and Linux
// daemons (B1). Validation rules, staged cooldown commit, and ready-edge
// emission have one owner here; each daemon keeps only platform glue
// (trace/live-channel/timer on Windows, triggered-reporting on Linux).

#include "layerc/core/automation.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

class CooldownEngine;

// An overlay override effect is well-formed: the property exists, the value
// fits its spec, and a timed secondary carries a duration.
[[nodiscard]] bool automation_overlay_effect_valid(
    const AutomationEffect &effect) noexcept;

// Effect-domain validation shared by both daemons: overlay effects must be
// well-formed and target a panel of the session; cooldown effects must carry
// durations where required and target a known cooldown or group; internal
// effects always validate.
[[nodiscard]] bool validate_automation_effect(
    const AutomationEffect &effect, std::span<const Panel> panels,
    std::span<const CooldownDefinition> cooldowns,
    std::span<const CooldownGroup> groups);

[[nodiscard]] bool validate_automation_cooldown_effect(
    const AutomationEffect &effect,
    std::span<const CooldownDefinition> cooldowns,
    std::span<const CooldownGroup> groups);

// Validate-then-apply commit of the winning effect set: every
// cooldown-domain effect is validated against the live definitions before
// anything mutates, so a failure never leaves a partial commit observable.
// Apply to a staged engine; a false return means nothing was applied.
[[nodiscard]] bool commit_automation_cooldown_effects(
    CooldownEngine &engine, std::span<const AutomationEffect> effects,
    std::span<const CooldownDefinition> cooldowns,
    std::span<const CooldownGroup> groups, TimePoint now,
    std::uint64_t origin_token);

// Earliest timer, overlay expiry, charge recovery, or persistent checkpoint.
[[nodiscard]] std::optional<TimePoint> next_automation_deadline(
    const AutomationRuntime &runtime, std::span<const CooldownState> cooldowns,
    std::optional<TimePoint> checkpoint = std::nullopt);

// Debounce between a persistent-value mutation and its durable checkpoint:
// bursts of timer/input writes collapse into one store write per second.
inline constexpr Milliseconds automation_checkpoint_debounce{1'000};

// Arms the debounce on first mutation; later mutations while armed keep the
// original deadline instead of postponing the checkpoint forever.
inline void automation_checkpoint_arm(std::optional<TimePoint> &due,
                                      const TimePoint now) noexcept {
  if (!due.has_value()) due = now + automation_checkpoint_debounce;
}

// Whether a durable write should be attempted now (force bypasses the
// deadline for shutdown/flush paths).
[[nodiscard]] inline bool automation_checkpoint_ready(
    const std::optional<TimePoint> &due, const TimePoint now,
    const bool force = false) noexcept {
  return due.has_value() && (force || *due <= now);
}

// Report the write outcome with one policy each way: success clears the
// deadline; failure slides it out by one debounce for a retry (never
// re-arms from the old deadline, so a persistently failing store cannot
// pin the checkpoint in the past and busy-loop the daemon).
inline void automation_checkpoint_report(std::optional<TimePoint> &due,
                                         const TimePoint now,
                                         const bool ok) noexcept {
  due = ok ? std::optional<TimePoint>{}
           : std::optional<TimePoint>{now + automation_checkpoint_debounce};
}

// Newly-ready ids since the previous poll. The runtime owns the edge maps;
// this updates them in place and reports what transitioned so the caller can
// emit cooldown_ready / group_ready events through its own event path.
struct ReadyTransitions {
  std::vector<std::string> cooldowns;
  std::vector<std::string> groups;
};

[[nodiscard]] ReadyTransitions poll_automation_ready_transitions(
    AutomationSessionEdges &edges,
    std::span<const CooldownState> snapshot,
    std::span<const CooldownGroup> groups);

} // namespace layerc::core
