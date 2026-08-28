#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {

inline constexpr std::size_t max_visual_json_bytes = 256U * 1024U;
inline constexpr std::size_t max_pixel_rules = 4;
inline constexpr std::size_t max_template_states = 4;
inline constexpr std::size_t max_template_references = 4;

enum class CaptureQuality : std::uint8_t {
  low,
  balanced,
  exact,
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

struct TemplateReference {
  // Lower-case SHA-256 digest. The platform asset store maps it to
  // _visual/assets/<digest>.png and verifies the contents before use.
  std::string asset_sha256;

  bool operator==(const TemplateReference &) const = default;
};

struct TemplateState {
  std::string id;
  std::string name;
  NormalizedRegion search_region;
  double expected_width{0.1};
  double expected_height{0.1};
  std::vector<TemplateReference> references;
  double confidence_threshold{0.90};
  double scale_tolerance{0.05};
  std::uint8_t color_tolerance{24};
  std::uint8_t stable_samples{2};
  PixelRepeatPolicy repeat{PixelRepeatPolicy::edge};

  bool operator==(const TemplateState &) const = default;
};

struct VisualTrigger {
  std::string cooldown_id;
  std::string observation_id;
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
  std::uint32_t schema_version{2};
  std::string profile_id;
  std::uint8_t sample_rate_hz{10};
  CaptureQuality quality{CaptureQuality::balanced};
  std::vector<PixelRule> rules;
  std::vector<TemplateState> templates;
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
[[nodiscard]] const PixelRule *
find_pixel_rule(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] const TemplateState *
find_template_state(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] bool
has_observation(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] std::uint8_t
observation_stable_samples(const VisualProfile &profile,
                           std::string_view id) noexcept;
[[nodiscard]] PixelRepeatPolicy
observation_repeat_policy(const VisualProfile &profile,
                          std::string_view id) noexcept;

} // namespace layerc::core
