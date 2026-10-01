#pragma once

#include "layerc/core/types.hpp"
#include "layerc/core/enum_wire.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::core {
namespace json { class Value; }

inline constexpr std::size_t max_visual_json_bytes = 256U * 1024U;
inline constexpr std::uint32_t visual_schema_version = 2;
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
  x11_composite,
  x11_full_frame,
};

enum class CaptureAvailability : std::uint8_t {
  inactive,
  starting,
  available,
  unavailable,
  protected_or_black,
  outside_output,
};

inline constexpr std::array capture_quality_wire{
    EnumWireEntry{CaptureQuality::low, "low"},
    EnumWireEntry{CaptureQuality::balanced, "balanced"},
    EnumWireEntry{CaptureQuality::exact, "exact"}};
inline constexpr std::array pixel_repeat_wire{
    EnumWireEntry{PixelRepeatPolicy::edge, "edge"},
    EnumWireEntry{PixelRepeatPolicy::when_fully_ready, "whenFullyReady"}};
inline constexpr std::array brightness_wire{
    EnumWireEntry{BrightnessComparison::above, "above"},
    EnumWireEntry{BrightnessComparison::below, "below"}};
inline constexpr std::array visual_trigger_mode_wire{
    EnumWireEntry{VisualTriggerMode::pixel_only, "pixelOnly"},
    EnumWireEntry{VisualTriggerMode::confirm_input, "confirmInput"}};
inline constexpr std::array capture_unavailable_policy_wire{
    EnumWireEntry{CaptureUnavailablePolicy::ignore, "ignore"},
    EnumWireEntry{CaptureUnavailablePolicy::trigger_from_input, "triggerFromInput"}};
inline constexpr std::array capture_availability_wire{
    EnumWireEntry{CaptureAvailability::inactive, "inactive"},
    EnumWireEntry{CaptureAvailability::starting, "starting"},
    EnumWireEntry{CaptureAvailability::available, "available"},
    EnumWireEntry{CaptureAvailability::unavailable, "unavailable"},
    EnumWireEntry{CaptureAvailability::protected_or_black, "protectedOrBlack"},
    EnumWireEntry{CaptureAvailability::outside_output, "outsideOutput"}};
inline constexpr std::array capture_backend_wire{
    EnumWireEntry{CaptureBackendKind::none, "none"},
    EnumWireEntry{CaptureBackendKind::windows_graphics_capture, "windowsGraphicsCapture"},
    EnumWireEntry{CaptureBackendKind::desktop_duplication, "desktopDuplication"},
    EnumWireEntry{CaptureBackendKind::x11_composite, "x11Composite"},
    EnumWireEntry{CaptureBackendKind::x11_full_frame, "x11FullFrame"}};

// Inclusive underlying maxima for schema and wire validation of these enums.
// Callers compare against these names instead of magic ordinals.
inline constexpr std::uint8_t capture_quality_max =
    static_cast<std::uint8_t>(CaptureQuality::exact);
inline constexpr std::uint8_t capture_backend_max =
    static_cast<std::uint8_t>(CaptureBackendKind::x11_full_frame);
inline constexpr std::uint8_t capture_availability_max =
    static_cast<std::uint8_t>(CaptureAvailability::outside_output);

// Frame-free, authoritative capture health shared by hosts and diagnostics.
// Host/service activity is independent of backend availability.
struct CaptureHealth {
  CaptureAvailability availability{CaptureAvailability::inactive};
  CaptureBackendKind backend{CaptureBackendKind::none};
  std::string diagnostic;

  bool operator==(const CaptureHealth &) const = default;
};

struct NormalizedRegion {
  double x{};
  double y{};
  double width{0.1};
  double height{0.1};

  bool operator==(const NormalizedRegion &) const = default;
};

// Capture and template availability share this tolerance. Profile validation
// keeps its existing, stricter tolerance to preserve accepted profile data.
inline constexpr double region_geometry_epsilon = 1e-9;

[[nodiscard]] inline bool contains(const NormalizedRegion &outer,
                                   const NormalizedRegion &inner) noexcept {
  return inner.x >= outer.x - region_geometry_epsilon &&
         inner.y >= outer.y - region_geometry_epsilon &&
         inner.x + inner.width <= outer.x + outer.width + region_geometry_epsilon &&
         inner.y + inner.height <= outer.y + outer.height + region_geometry_epsilon;
}

[[nodiscard]] inline bool overlaps(const NormalizedRegion &left,
                                   const NormalizedRegion &right) noexcept {
  return left.x < right.x + right.width - region_geometry_epsilon &&
         left.x + left.width > right.x + region_geometry_epsilon &&
         left.y < right.y + right.height - region_geometry_epsilon &&
         left.y + left.height > right.y + region_geometry_epsilon;
}

struct RegionPixelBounds {
  std::uint32_t left{}, top{}, right{}, bottom{};
};

[[nodiscard]] inline RegionPixelBounds to_pixel_bounds(
    const NormalizedRegion &region, const std::uint32_t width,
    const std::uint32_t height) noexcept {
  const auto edge = [](const double normalized, const std::uint32_t extent,
                       const bool upper) {
    const auto scaled = normalized * static_cast<double>(extent);
    return static_cast<std::uint32_t>(std::clamp(
        upper ? std::ceil(scaled) : std::floor(scaled), 0.0,
        static_cast<double>(extent)));
  };
  return {edge(region.x, width, false), edge(region.y, height, false),
          edge(region.x + region.width, width, true),
          edge(region.y + region.height, height, true)};
}

// Finite, nonempty, and within the 0..1 frame (inclusive, 1e-12 tolerance).
// Shared by profile validation and the calibration CLI so the accepted
// region set stays identical in both paths.
[[nodiscard]] bool
valid_normalized_region(const NormalizedRegion &region) noexcept;

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
  std::uint32_t schema_version{visual_schema_version};
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
// For a bounded document already accepted by json::parse (including UTF-8).
[[nodiscard]] VisualResult parse_visual_profile_value(
    const json::Value &root, const GameProfile &base_profile);

[[nodiscard]] std::string
serialize_visual_profile_json(const VisualProfile &profile);
[[nodiscard]] VisualError
validate_visual_profile(const VisualProfile &profile,
                        const GameProfile &base_profile,
                        const std::function<std::string(bool, std::size_t)> &source_path = {});
[[nodiscard]] bool
has_enabled_visual_assignments(const VisualProfile &profile) noexcept;
[[nodiscard]] const PixelRule *
find_pixel_rule(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] const TemplateState *
find_template_state(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] bool
has_observation(const VisualProfile &profile, std::string_view id) noexcept;
[[nodiscard]] PixelRepeatPolicy
observation_repeat_policy(const VisualProfile &profile,
                          std::string_view id) noexcept;

} // namespace layerc::core
