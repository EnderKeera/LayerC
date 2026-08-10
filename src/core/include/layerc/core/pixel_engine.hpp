#pragma once

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

struct RgbPixel {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};

  bool operator==(const RgbPixel &) const = default;
};

struct PixelFrameView {
  std::span<const RgbPixel> pixels;
  std::uint32_t width{};
  std::uint32_t height{};
  // Number of RgbPixel elements between the start of adjacent rows. Zero
  // means a tightly packed frame whose stride equals width.
  std::uint32_t row_stride{};

  [[nodiscard]] bool valid() const noexcept;
};

struct PixelEvaluation {
  bool matched{};
  std::uint32_t average_color{};
  double average_brightness{};
  double color_coverage{};
  std::size_t sampled_pixels{};
};

[[nodiscard]] PixelEvaluation evaluate_pixel_rule(
    const PixelRule &rule, PixelFrameView frame, CaptureQuality quality);

struct PixelRuleMeasurement {
  std::string rule_id;
  CaptureAvailability availability{CaptureAvailability::inactive};
  bool matched{};
  TimePoint sampled_at{};
};

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
  on_measurements(std::span<const PixelRuleMeasurement> measurements,
                  std::span<const CooldownState> cooldowns, TimePoint now);
  // The daemon calls this only after CooldownEngine accepts a request. It
  // rearms ready-repeat immediately, so a cooldown that starts and completes
  // between visual samples is still observable as a new ready cycle.
  void on_cooldown_triggered(std::string_view cooldown_id);

private:
  struct RuleState {
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

  [[nodiscard]] const PixelRule *find_rule(std::string_view id) const;
  [[nodiscard]] const VisualTrigger *
  find_trigger(std::string_view cooldown_id) const;
  [[nodiscard]] bool measurement_is_fresh(const RuleState &state,
                                          TimePoint now) const;
  [[nodiscard]] static bool is_terminal_unavailable(
      CaptureAvailability availability) noexcept;

  VisualProfile profile_;
  bool enabled_{true};
  std::unordered_map<std::string, RuleState> rules_;
  std::unordered_map<std::string, TriggerState> triggers_;
};

} // namespace layerc::core
