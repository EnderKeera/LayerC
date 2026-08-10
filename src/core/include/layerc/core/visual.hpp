#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

inline constexpr std::size_t max_visual_json_bytes = 64U * 1024U;
inline constexpr std::size_t max_pixel_rules = 4;

enum class CaptureQuality : std::uint8_t {
  low,
  balanced,
  exact,
};

enum class PixelPredicateKind : std::uint8_t {
  average_color,
  brightness,
  color_coverage,
};

enum class BrightnessComparison : std::uint8_t {
  above,
  below,
};

enum class PixelRepeatPolicy : std::uint8_t {
  edge,
  when_fully_ready,
};

enum class VisualTriggerMode : std::uint8_t {
  pixel_only,
  confirm_input,
};

enum class CaptureUnavailablePolicy : std::uint8_t {
  ignore,
  trigger_from_input,
};

enum class CaptureBackendKind : std::uint8_t {
  none,
  windows_graphics_capture,
  desktop_duplication,
};

enum class CaptureAvailability : std::uint8_t {
  inactive,
  starting,
  available,
  unavailable,
  protected_or_black,
  outside_output,
};

struct NormalizedRegion {
  double x{};
  double y{};
  double width{0.1};
  double height{0.1};

  bool operator==(const NormalizedRegion &) const = default;
};

struct AverageColorPredicate {
  std::uint32_t target_color{};
  std::uint8_t tolerance{16};

  bool operator==(const AverageColorPredicate &) const = default;
};

struct BrightnessPredicate {
  std::uint8_t threshold{128};
  BrightnessComparison comparison{BrightnessComparison::above};

  bool operator==(const BrightnessPredicate &) const = default;
};

struct ColorCoveragePredicate {
  std::uint32_t target_color{};
  std::uint8_t tolerance{16};
  double minimum_fraction{0.5};

  bool operator==(const ColorCoveragePredicate &) const = default;
};

using PixelPredicate =
    std::variant<AverageColorPredicate, BrightnessPredicate,
                 ColorCoveragePredicate>;

struct PixelRule {
  std::string id;
  std::string name;
  NormalizedRegion region;
  PixelPredicate predicate;
  std::uint8_t stable_samples{2};
  PixelRepeatPolicy repeat{PixelRepeatPolicy::edge};

  bool operator==(const PixelRule &) const = default;
};

struct VisualTrigger {
  std::string cooldown_id;
  std::string rule_id;
  bool enabled{true};
  VisualTriggerMode mode{VisualTriggerMode::pixel_only};
  // Pixel-only assignments suppress the underlying input binding when false.
  bool keep_input{false};
  Milliseconds confirmation_timeout{500};
  CaptureUnavailablePolicy unavailable_policy{
      CaptureUnavailablePolicy::ignore};

  bool operator==(const VisualTrigger &) const = default;
};

struct VisualProfile {
  std::uint32_t schema_version{1};
  std::string profile_id;
  std::uint8_t sample_rate_hz{10};
  CaptureQuality quality{CaptureQuality::balanced};
  std::vector<PixelRule> rules;
  std::vector<VisualTrigger> triggers;

  bool operator==(const VisualProfile &) const = default;
};

struct VisualError {
  std::string path;
  std::string message;
};

using VisualResult = std::variant<VisualProfile, VisualError>;

[[nodiscard]] VisualResult
parse_visual_profile_json(std::string_view source,
                          const GameProfile &base_profile);
[[nodiscard]] std::string
serialize_visual_profile_json(const VisualProfile &profile);
[[nodiscard]] VisualError
validate_visual_profile(const VisualProfile &profile,
                        const GameProfile &base_profile);
[[nodiscard]] bool
has_enabled_visual_assignments(const VisualProfile &profile) noexcept;

[[nodiscard]] constexpr PixelPredicateKind
predicate_kind(const PixelPredicate &predicate) noexcept {
  if (std::holds_alternative<AverageColorPredicate>(predicate))
    return PixelPredicateKind::average_color;
  if (std::holds_alternative<BrightnessPredicate>(predicate))
    return PixelPredicateKind::brightness;
  return PixelPredicateKind::color_coverage;
}

} // namespace layerc::core
