#pragma once

#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace layerc::core {

enum class VisualTriggerSource : std::uint8_t {
  direct_input,
  pixel_edge,
  pixel_repeat,
  input_confirmed,
  capture_fallback,
};

struct VisualTriggerRequest {
  std::string cooldown_id;
  VisualTriggerSource source{VisualTriggerSource::direct_input};

  bool operator==(const VisualTriggerRequest &) const = default;
};

// Converts input edges and debounced pixel measurements into cooldown trigger
// requests. It deliberately does not own CooldownEngine: the daemon remains
// the single authority for consuming charges and applying retrigger policy.
class VisualTriggerEngine {
public:
  explicit VisualTriggerEngine(const VisualProfile &profile);

  void reconfigure(const VisualProfile &profile);
  void set_visual_enabled(bool enabled);
  [[nodiscard]] bool visual_enabled() const noexcept { return enabled_; }
  void clear_transient_state();

  [[nodiscard]] std::vector<VisualTriggerRequest>
  on_input(std::string_view cooldown_id, TimePoint now);
  [[nodiscard]] std::vector<VisualTriggerRequest>
  on_measurements(std::span<const ObservationMeasurement> measurements,
                  std::span<const CooldownState> cooldowns, TimePoint now);
  // The daemon calls this only after CooldownEngine accepts a request. It
  // rearms ready-repeat immediately, so a cooldown that starts and completes
  // between visual samples is still observable as a new ready cycle.
  void on_cooldown_triggered(std::string_view cooldown_id);

private:
  struct RuleState {
    std::uint8_t stable_samples{1};
    std::uint8_t matching_samples{};
    std::uint8_t nonmatching_samples{};
    bool stable_match{};
    CaptureAvailability availability{CaptureAvailability::inactive};
    TimePoint sampled_at{};
    bool has_sample{};
  };

  struct TriggerState {
    bool edge_armed{true};
    bool repeat_fired_for_ready{};
    bool pending_confirmation{};
    TimePoint confirmation_deadline{};
  };

  enum class ConfirmationDecision : std::uint8_t {
    wait,
    confirmed,
    fallback,
    terminal,
  };

  [[nodiscard]] const VisualTrigger *
  find_trigger(std::string_view cooldown_id) const;
  [[nodiscard]] ConfirmationDecision
  classify(const VisualTrigger &trigger, const RuleState *state,
           TimePoint now) const;
  [[nodiscard]] bool measurement_is_fresh(const RuleState &state,
                                          TimePoint now) const;
  [[nodiscard]] static bool is_terminal_unavailable(
      CaptureAvailability availability) noexcept;
  void apply_measurement(const ObservationMeasurement &measurement);
  void evaluate_confirm_trigger(const VisualTrigger &trigger,
                                TriggerState &trigger_state,
                                const RuleState &rule_state, TimePoint now,
                                std::vector<VisualTriggerRequest> &output);
  void evaluate_pixel_trigger(const VisualTrigger &trigger,
                              TriggerState &trigger_state,
                              const RuleState &rule_state,
                              std::span<const CooldownState> cooldowns,
                              TimePoint now,
                              std::vector<VisualTriggerRequest> &output);

  VisualProfile profile_;
  bool enabled_{true};
  std::unordered_map<std::string, RuleState> rules_;
  std::unordered_map<std::string, TriggerState> triggers_;
  std::unordered_map<std::string, std::size_t> trigger_indices_;
};

} // namespace layerc::core
