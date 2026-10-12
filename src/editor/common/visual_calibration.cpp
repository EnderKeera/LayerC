#include "visual_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <utility>

namespace layerc::editor {
namespace {

using layerc::core::NormalizedRegion;

std::uint8_t byte_value(const int value, const int minimum = 0) {
  return static_cast<std::uint8_t>(std::clamp(value, minimum, 255));
}

std::uint8_t stable_value(const int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 1, 15));
}

bool contains_binding(const layerc::core::GameProfile &profile,
                      const std::string_view cooldown_id) {
  return std::ranges::any_of(profile.bindings, [&](const auto &binding) {
    return binding.cooldown_id == cooldown_id;
  });
}

} // namespace

NormalizedRegion expanded_template_search_region(const NormalizedRegion selected) {
  const auto expand = [](const double origin, const double extent) {
    const double expanded = std::min(1.0, extent * 2.0);
    const double center = origin + extent * 0.5;
    return std::pair{std::clamp(center - expanded * 0.5, 0.0, 1.0 - expanded),
                     expanded};
  };
  const auto [x, width] = expand(selected.x, selected.width);
  const auto [y, height] = expand(selected.y, selected.height);
  return {x, y, width, height};
}

std::optional<layerc::core::TemplateImage>
template_from_selection(const layerc::core::PixelFrameView frame,
                        const NormalizedRegion &selection) {
  if (!frame.valid()) return std::nullopt;
  const auto [left, top, right, bottom] =
      layerc::core::to_pixel_bounds(selection, frame.width, frame.height);
  if (right <= left || bottom <= top ||
      static_cast<std::uint64_t>(right - left) * (bottom - top) < 64U)
    return std::nullopt;
  const auto source_width = right - left, source_height = bottom - top;
  const auto source_offset = [](const std::uint32_t index,
                                const std::uint32_t extent,
                                const std::uint32_t source_extent) {
    return std::min(source_extent - 1,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(index) * 2U + 1U) *
                        source_extent / (static_cast<std::uint64_t>(extent) * 2U)));
  };
  const double factor =
      std::min(1.0, static_cast<double>(layerc::core::max_template_axis) /
                        std::max(source_width, source_height));
  layerc::core::TemplateImage image;
  image.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_width * factor)));
  image.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_height * factor)));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  image.alpha.assign(image.pixels.size(), 255);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto source_y = top + source_offset(y, image.height, source_height);
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source_x = left + source_offset(x, image.width, source_width);
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          frame.pixels[static_cast<std::size_t>(source_y) * frame.stride() +
                       source_x];
    }
  }
  return image;
}

bool paint_template_mask(layerc::core::TemplateImage &image, const double x,
                         const double y, const bool opaque) {
  if (image.width == 0 || image.height == 0 || !(x >= 0.0) || !(x < 1.0) ||
      !(y >= 0.0) || !(y < 1.0) ||
      image.alpha.size() != static_cast<std::size_t>(image.width) * image.height)
    return false;
  const auto column = static_cast<int>(x * image.width);
  const auto row = static_cast<int>(y * image.height);
  const int radius = std::max<int>(
      1, static_cast<int>(std::min(image.width, image.height) / 32));
  for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
    for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
      if (offset_x * offset_x + offset_y * offset_y > radius * radius) continue;
      const int target_x = column + offset_x, target_y = row + offset_y;
      if (target_x < 0 || target_y < 0 ||
          target_x >= static_cast<int>(image.width) ||
          target_y >= static_cast<int>(image.height))
        continue;
      image.alpha[static_cast<std::size_t>(target_y) * image.width +
                  static_cast<std::size_t>(target_x)] = opaque ? 255 : 0;
    }
  }
  return true;
}

std::optional<CalibrationModel>
CalibrationModel::open(const layerc::core::GameProfile &profile,
                       const std::string_view cooldown_id,
                       const std::optional<layerc::core::VisualProfile> &existing) {
  const auto cooldown = std::ranges::find(
      profile.cooldowns, cooldown_id, &layerc::core::CooldownDefinition::id);
  if (cooldown == profile.cooldowns.end()) return std::nullopt;
  CalibrationModel model;
  model.profile_ = profile;
  auto &controls = model.initial_controls_;
  if (existing) {
    controls.sample_rate_hz = existing->sample_rate_hz;
    controls.quality = existing->quality;
  }
  model.trigger_ = {.cooldown_id = cooldown->id,
                    .observation_id = cooldown->id + "-visual",
                    .enabled = true,
                    .mode = layerc::core::VisualTriggerMode::pixel_only,
                    .keep_input = false};
  model.rule_ = {.id = model.trigger_.observation_id,
                 .name = cooldown->name + " Ready",
                 .region = {0.4, 0.4, 0.2, 0.2},
                 .predicate = layerc::core::AverageColorPredicate{0xFFFFFF, 16}};
  model.template_state_ = {.id = model.trigger_.observation_id,
                           .name = cooldown->name + " Ready",
                           .search_region = model.rule_.region,
                           .expected_width = model.rule_.region.width,
                           .expected_height = model.rule_.region.height,
                           .references = {}};
  bool existing_template = false;
  if (existing) {
    const auto assignment = std::ranges::find(
        existing->triggers, cooldown->id, &layerc::core::VisualTrigger::cooldown_id);
    if (assignment != existing->triggers.end()) {
      model.trigger_ = *assignment;
      model.rule_.id = model.trigger_.observation_id;
      model.template_state_.id = model.trigger_.observation_id;
      const auto *observation =
          layerc::core::find_observation(*existing, model.trigger_.observation_id);
      if (const auto *rule = std::get_if<layerc::core::PixelRule>(observation)) {
        model.rule_ = *rule;
      } else if (const auto *state =
                     std::get_if<layerc::core::TemplateState>(observation)) {
        model.template_state_ = *state;
        existing_template = true;
        model.restore_reference_selection();
      }
    } else {
      const std::string base = model.trigger_.observation_id;
      for (unsigned suffix = 2;
           layerc::core::find_observation(*existing, model.trigger_.observation_id);
           ++suffix)
        model.trigger_.observation_id = base + "-" + std::to_string(suffix);
      model.rule_.id = model.trigger_.observation_id;
      model.template_state_.id = model.trigger_.observation_id;
    }
  }
  if (!existing_template) model.set_selection(model.rule_.region);

  if (const auto *average =
          std::get_if<layerc::core::AverageColorPredicate>(&model.rule_.predicate)) {
    model.target_color_ = average->target_color;
    controls.tolerance = average->tolerance;
  } else if (const auto *brightness =
                 std::get_if<layerc::core::BrightnessPredicate>(&model.rule_.predicate)) {
    controls.predicate = CalibrationPredicate::brightness;
    controls.threshold = brightness->threshold;
    controls.comparison = brightness->comparison;
  } else if (const auto *coverage =
                 std::get_if<layerc::core::ColorCoveragePredicate>(
                     &model.rule_.predicate)) {
    model.target_color_ = coverage->target_color;
    controls.predicate = CalibrationPredicate::coverage;
    controls.tolerance = coverage->tolerance;
    controls.minimum_percent =
        static_cast<int>(std::lround(coverage->minimum_fraction * 100.0));
  }
  if (!model.template_state_.references.empty()) {
    controls.predicate = CalibrationPredicate::template_match;
    controls.tolerance = model.template_state_.color_tolerance;
  }
  const bool template_mode = controls.predicate == CalibrationPredicate::template_match;
  controls.repeat = template_mode ? model.template_state_.repeat : model.rule_.repeat;
  controls.stable_samples = template_mode ? model.template_state_.stable_samples
                                          : model.rule_.stable_samples;
  controls.confidence_percent = static_cast<int>(
      std::lround(model.template_state_.confidence_threshold * 100.0));
  controls.scale_percent =
      static_cast<int>(std::lround(model.template_state_.scale_tolerance * 100.0));
  controls.trigger_mode = model.trigger_.mode;
  controls.keep_input = model.trigger_.keep_input;
  controls.timeout_ms = static_cast<int>(model.trigger_.confirmation_timeout.count());
  controls.unavailable = model.trigger_.unavailable_policy;
  return model;
}

void CalibrationModel::set_selection(const NormalizedRegion &selected) {
  selection_ = selected;
  rule_.region = selected;
  template_state_.expected_width = selected.width;
  template_state_.expected_height = selected.height;
  template_state_.search_region = expanded_template_search_region(selected);
}

void CalibrationModel::restore_reference_selection() {
  // Older profiles persist dimensions/search, not the original crop origin.
  // Seed a same-sized central crop without changing persisted geometry.
  const auto &search = template_state_.search_region;
  const auto width = std::isfinite(template_state_.expected_width)
      ? std::clamp(template_state_.expected_width, 0.0, 1.0) : 0.0;
  const auto height = std::isfinite(template_state_.expected_height)
      ? std::clamp(template_state_.expected_height, 0.0, 1.0) : 0.0;
  const auto x = search.x + (search.width - width) * 0.5;
  const auto y = search.y + (search.height - height) * 0.5;
  selection_ = {std::isfinite(x) ? std::clamp(x, 0.0, 1.0 - width) : 0.0,
                std::isfinite(y) ? std::clamp(y, 0.0, 1.0 - height) : 0.0,
                width, height};
  rule_.region = selection_;
}

std::optional<std::uint32_t>
CalibrationModel::sample_color(const layerc::core::PixelFrameView frame,
                               const layerc::core::CaptureQuality quality) const {
  auto sample = rule_;
  sample.predicate = layerc::core::AverageColorPredicate{0, 255};
  const auto evaluation = layerc::core::evaluate_pixel_rule(sample, frame, quality);
  if (evaluation.sampled_pixels == 0) return std::nullopt;
  return evaluation.average_color;
}

layerc::core::PixelRule
CalibrationModel::pixel_rule(const CalibrationControls &controls) const {
  auto rule = rule_;
  rule.stable_samples = stable_value(controls.stable_samples);
  rule.repeat = controls.repeat;
  switch (controls.predicate) {
  case CalibrationPredicate::brightness:
    rule.predicate = layerc::core::BrightnessPredicate{byte_value(controls.threshold),
                                                       controls.comparison};
    break;
  case CalibrationPredicate::coverage: {
    const auto *original =
        std::get_if<layerc::core::ColorCoveragePredicate>(&rule_.predicate);
    const double fraction = original &&
        controls.minimum_percent == initial_controls_.minimum_percent
        ? original->minimum_fraction
        : std::clamp(controls.minimum_percent, 0, 100) / 100.0;
    rule.predicate = layerc::core::ColorCoveragePredicate{
        target_color_, byte_value(controls.tolerance), fraction};
    break;
  }
  case CalibrationPredicate::average_color:
  case CalibrationPredicate::template_match:
    rule.predicate = layerc::core::AverageColorPredicate{
        target_color_, byte_value(controls.tolerance)};
    break;
  }
  return rule;
}

layerc::core::TemplateState
CalibrationModel::template_state(const CalibrationControls &controls) const {
  auto state = template_state_;
  if (controls.confidence_percent != initial_controls_.confidence_percent)
    state.confidence_threshold = std::clamp(controls.confidence_percent, 0, 100) / 100.0;
  if (controls.scale_percent != initial_controls_.scale_percent)
    state.scale_tolerance = std::clamp(controls.scale_percent, 0, 20) / 100.0;
  state.color_tolerance = byte_value(controls.tolerance);
  state.stable_samples = stable_value(controls.stable_samples);
  state.repeat = controls.repeat;
  return state;
}

bool CalibrationModel::add_reference(const layerc::core::TemplateReference &reference) {
  auto &references = template_state_.references;
  if (references.size() >= layerc::core::max_template_references ||
      std::ranges::find(references, reference) != references.end())
    return false;
  references.push_back(reference);
  return true;
}

bool CalibrationModel::replace_last_reference(
    const layerc::core::TemplateReference &reference) {
  auto &references = template_state_.references;
  if (references.empty() ||
      std::find(references.begin(), references.end() - 1, reference) !=
          references.end() - 1)
    return false;
  references.back() = reference;
  return true;
}

std::variant<layerc::core::VisualProfile, std::string>
CalibrationModel::build_result(
    const CalibrationControls &controls,
    const std::span<const layerc::core::TemplateImage> images) const {
  const bool use_template = controls.predicate == CalibrationPredicate::template_match;
  layerc::core::VisualProfile result;
  result.profile_id = profile_.id;
  result.sample_rate_hz = controls.sample_rate_hz;
  result.quality = controls.quality;
  if (use_template) {
    auto state = template_state(controls);
    state.id = trigger_.observation_id;
    if (state.references.empty())
      return std::string("Capture or import at least one template reference first.");
    if (images.size() != state.references.size() ||
        !std::ranges::all_of(images, layerc::core::template_image_has_structure))
      return std::string("Every referenced template asset must be available and "
                         "contain visible visual structure.");
    for (std::size_t index{}; index < images.size(); ++index)
      if (images[index].asset_sha256 != state.references[index].asset_sha256)
        return std::string("Template images must match their referenced assets in order.");
    result.observations.emplace_back(std::move(state));
  } else {
    auto rule = pixel_rule(controls);
    rule.id = trigger_.observation_id;
    result.observations.emplace_back(std::move(rule));
  }
  auto trigger = trigger_;
  trigger.enabled = true;
  trigger.mode = controls.trigger_mode;
  trigger.keep_input = controls.keep_input;
  trigger.confirmation_timeout =
      layerc::core::Milliseconds(std::clamp(controls.timeout_ms, 50, 5'000));
  trigger.unavailable_policy = controls.unavailable;
  if (trigger.mode == layerc::core::VisualTriggerMode::confirm_input &&
      !contains_binding(profile_, trigger.cooldown_id))
    return std::string(
        "Input confirmation requires a captured input binding for this cooldown.");
  result.triggers.push_back(std::move(trigger));
  const auto validation = layerc::core::validate_visual_profile(result, profile_);
  if (!validation.message.empty()) return validation.message;
  return result;
}

} // namespace layerc::editor
