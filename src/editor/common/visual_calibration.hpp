#pragma once

#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/core/visual.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::editor {

// The toolkit-free model behind one graphical calibration: the selected
// cooldown's draft observation and trigger, the explicit reference selection
// and template images. Frontends own capture, preview and dialogs.
enum class CalibrationPredicate : std::uint8_t {
  average_color,
  brightness,
  coverage,
  template_match,
};

// Control values with the Windows calibration defaults.
struct CalibrationControls {
  CalibrationPredicate predicate{CalibrationPredicate::average_color};
  int tolerance{16};
  int threshold{128};
  layerc::core::BrightnessComparison comparison{
      layerc::core::BrightnessComparison::above};
  int minimum_percent{50};
  int confidence_percent{90};
  int scale_percent{5};
  int stable_samples{2};
  layerc::core::PixelRepeatPolicy repeat{layerc::core::PixelRepeatPolicy::edge};
  std::uint8_t sample_rate_hz{10};
  layerc::core::CaptureQuality quality{layerc::core::CaptureQuality::balanced};
  layerc::core::VisualTriggerMode trigger_mode{
      layerc::core::VisualTriggerMode::pixel_only};
  bool keep_input{};
  int timeout_ms{500};
  layerc::core::CaptureUnavailablePolicy unavailable{
      layerc::core::CaptureUnavailablePolicy::ignore};

  bool operator==(const CalibrationControls &) const = default;
};

// Centered search area twice the selection on each axis, within the frame.
[[nodiscard]] layerc::core::NormalizedRegion
expanded_template_search_region(layerc::core::NormalizedRegion selected);

// Samples the source pixel under the center of each downscaled pixel so the
// image never exceeds core::max_template_axis on its longer side. Nullopt
// when the selection covers fewer than 64 captured pixels.
[[nodiscard]] std::optional<layerc::core::TemplateImage>
template_from_selection(layerc::core::PixelFrameView frame,
                        const layerc::core::NormalizedRegion &selection);

// Paints a round brush into the alpha mask at normalized image coordinates:
// opaque restores pixels, transparent excludes them from matching.
[[nodiscard]] bool paint_template_mask(layerc::core::TemplateImage &image,
                                       double x, double y, bool opaque);

class CalibrationModel final {
public:
  // Seeds from the cooldown's existing assignment, or a fresh "<id>-visual"
  // observation unique within `existing`. Nullopt for an unknown cooldown.
  [[nodiscard]] static std::optional<CalibrationModel>
  open(const layerc::core::GameProfile &profile, std::string_view cooldown_id,
       const std::optional<layerc::core::VisualProfile> &existing);

  [[nodiscard]] const layerc::core::GameProfile &profile() const noexcept {
    return profile_;
  }
  [[nodiscard]] const layerc::core::VisualTrigger &trigger() const noexcept {
    return trigger_;
  }
  [[nodiscard]] const layerc::core::NormalizedRegion &selection() const noexcept {
    return selection_;
  }
  [[nodiscard]] const std::vector<layerc::core::TemplateReference> &
  references() const noexcept {
    return template_state_.references;
  }
  [[nodiscard]] std::uint32_t target_color() const noexcept {
    return target_color_;
  }
  // The controls that reproduce the seeded assignment.
  [[nodiscard]] const CalibrationControls &initial_controls() const noexcept {
    return initial_controls_;
  }

  void set_selection(const layerc::core::NormalizedRegion &selected);
  void set_target_color(std::uint32_t color) noexcept { target_color_ = color; }
  // The average color inside the selection, or nullopt with no pixels.
  [[nodiscard]] std::optional<std::uint32_t>
  sample_color(layerc::core::PixelFrameView frame,
               layerc::core::CaptureQuality quality) const;

  [[nodiscard]] layerc::core::PixelRule
  pixel_rule(const CalibrationControls &controls) const;
  [[nodiscard]] layerc::core::TemplateState
  template_state(const CalibrationControls &controls) const;

  // False when the reference limit is reached or the asset is already used.
  [[nodiscard]] bool add_reference(const layerc::core::TemplateReference &reference);
  // Replaces the newest reference after its mask changed; false when that
  // duplicates an earlier one.
  [[nodiscard]] bool
  replace_last_reference(const layerc::core::TemplateReference &reference);
  void clear_references() noexcept { template_state_.references.clear(); }

  // The one-observation, one-trigger result for the selected cooldown, or
  // the user-facing reason it cannot be applied. Template mode requires every
  // reference image loaded with visible structure.
  [[nodiscard]] std::variant<layerc::core::VisualProfile, std::string>
  build_result(const CalibrationControls &controls,
               std::span<const layerc::core::TemplateImage> images) const;

private:
  void restore_reference_selection();

  layerc::core::GameProfile profile_;
  layerc::core::PixelRule rule_;
  layerc::core::TemplateState template_state_;
  layerc::core::VisualTrigger trigger_;
  // The crop belongs to the explicit selection, never the expanded search area.
  layerc::core::NormalizedRegion selection_;
  std::uint32_t target_color_{};
  CalibrationControls initial_controls_;
};

} // namespace layerc::editor
