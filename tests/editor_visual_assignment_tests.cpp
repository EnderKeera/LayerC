#include "profile_edit.hpp"
#include "test_support.hpp"
#include "visual_assignment.hpp"
#include "visual_calibration.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {
using namespace layerc::core;
using namespace layerc::editor;
using layerc::test::expect;

GameProfile base_profile() {
  auto profile = make_new_profile();
  profile.game.executable_name = "fixture.exe";
  auto second = profile.cooldowns.front();
  second.id += "-second";
  second.name += " second";
  profile.cooldowns.push_back(second);
  return profile;
}

template <typename Value, typename Variant>
const Value *as(const Variant &result) {
  return std::get_if<Value>(&result);
}

std::string calibration_json(const GameProfile &profile, const std::string &cooldown,
                             const std::string &observation,
                             const std::uint32_t color) {
  auto model = CalibrationModel::open(profile, cooldown, std::nullopt);
  if (!model) return {};
  model->set_target_color(color);
  auto controls = model->initial_controls();
  auto built = model->build_result(controls, {});
  auto *visual = std::get_if<VisualProfile>(&built);
  if (visual == nullptr) return {};
  std::visit([&](auto &item) { item.id = observation; }, visual->observations[0]);
  visual->triggers[0].observation_id = observation;
  return serialize_visual_profile_json(*visual);
}

void model_seeds_and_builds() {
  const auto profile = base_profile();
  const auto &cooldown = profile.cooldowns.front();
  expect(!CalibrationModel::open(profile, "missing", std::nullopt),
         "an unknown cooldown has no calibration");
  auto model = CalibrationModel::open(profile, cooldown.id, std::nullopt);
  if (!expect(model.has_value(), "a known cooldown opens")) return;
  expect(model->trigger().observation_id == cooldown.id + "-visual" &&
             model->selection() == NormalizedRegion{0.4, 0.4, 0.2, 0.2} &&
             model->initial_controls() == CalibrationControls{},
         "a fresh calibration uses the Windows defaults");

  model->set_target_color(0x112233);
  auto controls = model->initial_controls();
  controls.predicate = CalibrationPredicate::coverage;
  controls.tolerance = 300;
  controls.minimum_percent = 75;
  controls.stable_samples = 0;
  const auto rule = model->pixel_rule(controls);
  const auto *coverage = std::get_if<ColorCoveragePredicate>(&rule.predicate);
  expect(coverage != nullptr && coverage->target_color == 0x112233 &&
             coverage->tolerance == 255 && coverage->minimum_fraction == 0.75 &&
             rule.stable_samples == 1,
         "controls are clamped into the pixel rule");

  controls.predicate = CalibrationPredicate::brightness;
  controls.threshold = 40;
  controls.comparison = BrightnessComparison::below;
  controls.timeout_ms = 1;
  const auto built = model->build_result(controls, {});
  const auto *visual = as<VisualProfile>(built);
  if (!expect(visual != nullptr, "a brightness rule builds")) return;
  const auto *brightness = std::get_if<BrightnessPredicate>(
      &std::get<PixelRule>(visual->observations[0]).predicate);
  expect(visual->triggers.size() == 1 && visual->triggers[0].cooldown_id == cooldown.id &&
             visual->triggers[0].confirmation_timeout == Milliseconds(50) &&
             brightness != nullptr && brightness->threshold == 40 &&
             brightness->comparison == BrightnessComparison::below,
         "the result carries one observation and one clamped trigger");

  controls.predicate = CalibrationPredicate::template_match;
  expect(as<std::string>(model->build_result(controls, {})) != nullptr,
         "template mode needs a reference");
  auto unbound = profile;
  unbound.bindings.clear();
  auto confirm = CalibrationModel::open(unbound, cooldown.id, std::nullopt);
  auto confirm_controls = confirm->initial_controls();
  confirm_controls.trigger_mode = VisualTriggerMode::confirm_input;
  const auto refused = confirm->build_result(confirm_controls, {});
  expect(as<std::string>(refused) != nullptr &&
             as<std::string>(refused)->find("input binding") != std::string::npos,
         "input confirmation needs a binding");
}

void model_reopens_existing_assignment() {
  const auto profile = base_profile();
  const auto &cooldown = profile.cooldowns.front();
  auto model = CalibrationModel::open(profile, cooldown.id, std::nullopt);
  model->set_target_color(0x00ff00);
  model->set_selection({0.1, 0.2, 0.3, 0.4});
  auto controls = model->initial_controls();
  controls.predicate = CalibrationPredicate::coverage;
  controls.minimum_percent = 60;
  controls.sample_rate_hz = 15;
  controls.quality = CaptureQuality::low;
  controls.keep_input = true;
  auto visual = std::get<VisualProfile>(model->build_result(controls, {}));
  std::get<ColorCoveragePredicate>(std::get<PixelRule>(visual.observations.front()).predicate)
      .minimum_fraction = 0.604;
  const auto reopened = CalibrationModel::open(profile, cooldown.id, visual);
  expect(reopened && reopened->initial_controls() == controls &&
             reopened->target_color() == 0x00ff00 &&
             reopened->selection() == NormalizedRegion{0.1, 0.2, 0.3, 0.4},
         "an existing assignment reproduces its controls");
  expect(reopened->pixel_rule(reopened->initial_controls()).predicate ==
             std::get<PixelRule>(visual.observations.front()).predicate,
         "untouched coverage preserves a fractional percentage exactly");
  auto changed = reopened->initial_controls();
  changed.minimum_percent = 61;
  expect(std::get<ColorCoveragePredicate>(reopened->pixel_rule(changed).predicate)
             .minimum_fraction == 0.61,
         "an explicitly changed coverage control replaces the original fraction");

  const auto other = CalibrationModel::open(profile, profile.cooldowns[1].id, visual);
  expect(other && other->trigger().observation_id == profile.cooldowns[1].id + "-visual",
         "another cooldown gets its own observation id");
  visual.observations.push_back(PixelRule{.id = profile.cooldowns[1].id + "-visual"});
  const auto clash = CalibrationModel::open(profile, profile.cooldowns[1].id, visual);
  expect(clash && clash->trigger().observation_id == profile.cooldowns[1].id + "-visual-2",
         "a used observation id is suffixed");
}

void template_selection_and_mask() {
  std::vector<RgbPixel> pixels(40 * 20);
  for (std::size_t index = 0; index < pixels.size(); ++index)
    pixels[index] = RgbPixel{static_cast<std::uint8_t>(index % 40), 0,
                             static_cast<std::uint8_t>(index / 40)};
  const PixelFrameView frame{pixels, 40, 20, 40};
  expect(!template_from_selection(frame, {0.0, 0.0, 0.1, 0.1}),
         "a selection under 64 pixels is refused");
  const auto image = template_from_selection(frame, {0.5, 0.5, 0.5, 0.5});
  if (!expect(image && image->width == 20 && image->height == 10 &&
                  image->pixels[0] == pixels[10 * 40 + 20] &&
                  image->alpha.size() == 200,
              "the selection is cropped at full resolution"))
    return;
  std::vector<RgbPixel> wide(1200 * 10);
  const auto scaled =
      template_from_selection({wide, 1200, 10, 1200}, {0.0, 0.0, 1.0, 1.0});
  expect(scaled && scaled->width == max_template_axis && scaled->height == 4,
         "a long selection is downscaled to the template axis limit");

  auto masked = *image;
  expect(paint_template_mask(masked, 0.0, 0.0, false) && masked.alpha[0] == 0 &&
             masked.alpha[199] == 255,
         "the brush clears alpha near the point");
  expect(!paint_template_mask(masked, 1.0, 0.5, false),
         "points outside the image are ignored");

  const auto search = expanded_template_search_region({0.8, 0.1, 0.2, 0.2});
  expect(search == NormalizedRegion{0.6, 0.0, 0.4, 0.4},
         "the search area doubles and stays inside the frame");

  auto model = CalibrationModel::open(base_profile(), base_profile().cooldowns[0].id,
                                      std::nullopt);
  for (int index = 0; index < 4; ++index)
    expect(model->add_reference({std::string(64, static_cast<char>('a' + index))}),
           "up to four references are accepted");
  expect(!model->add_reference({std::string(64, 'e')}) &&
             !model->replace_last_reference({std::string(64, 'a')}) &&
             model->replace_last_reference({std::string(64, 'f')}) &&
             model->references().back().asset_sha256 == std::string(64, 'f'),
         "the limit and duplicate references are refused");
}

void template_reopen_and_asset_validation() {
  const auto profile = base_profile();
  const auto &cooldown = profile.cooldowns.front();
  auto model = CalibrationModel::open(profile, cooldown.id, std::nullopt);
  auto controls = model->initial_controls();
  controls.predicate = CalibrationPredicate::template_match;
  TemplateImage image;
  image.width = image.height = 32;
  image.pixels.resize(32 * 32);
  image.alpha.assign(image.pixels.size(), 255);
  image.asset_sha256.assign(64, 'a');
  for (std::size_t index{}; index < image.pixels.size(); ++index)
    image.pixels[index] = (index + index / 32) % 2 == 0
                             ? RgbPixel{0, 0, 0} : RgbPixel{255, 255, 255};
  expect(model->add_reference({image.asset_sha256}), "a structured template reference is accepted");
  const std::vector<TemplateImage> images{image};
  auto built = model->build_result(controls, images);
  auto *visual = std::get_if<VisualProfile>(&built);
  if (!expect(visual != nullptr, "a structured template builds")) return;
  auto &state = std::get<TemplateState>(visual->observations.front());
  state.search_region = {0.07, 0.11, 0.56, 0.51};
  state.expected_width = 0.12;
  state.expected_height = 0.16;
  state.confidence_threshold = 0.904;
  state.scale_tolerance = 0.054;
  visual->triggers.front().enabled = false;
  const auto reopened = CalibrationModel::open(profile, cooldown.id, *visual);
  if (!expect(reopened.has_value(), "an existing template reopens")) return;
  expect(reopened->template_state(reopened->initial_controls()) == state,
         "opening a template preserves geometry and fractional thresholds exactly");
  auto changed = reopened->initial_controls();
  changed.confidence_percent = 91;
  changed.scale_percent = 6;
  const auto edited = reopened->template_state(changed);
  expect(edited.confidence_threshold == 0.91 && edited.scale_tolerance == 0.06,
         "only changed template percentage controls replace original fractions");
  const auto result = reopened->build_result(reopened->initial_controls(), images);
  const auto *saved = std::get_if<VisualProfile>(&result);
  expect(saved && saved->observations.front() == visual->observations.front() &&
             saved->triggers.front().enabled,
         "Apply keeps untouched template geometry and explicitly enables the assignment");
  expect(as<std::string>(reopened->build_result(reopened->initial_controls(), {})) != nullptr,
         "unavailable referenced images cannot be silently discarded");
  auto wrong = images;
  wrong.front().asset_sha256.assign(64, 'b');
  expect(as<std::string>(reopened->build_result(reopened->initial_controls(), wrong)) != nullptr,
         "a structurally valid image for another digest is refused");
  auto pixel_controls = reopened->initial_controls();
  pixel_controls.predicate = CalibrationPredicate::brightness;
  const auto pixel = reopened->pixel_rule(pixel_controls);
  expect(pixel.region == reopened->selection() && valid_normalized_region(pixel.region),
         "changing from an existing template to pixels retains a valid recovered selection");
}

void merge_new_shared_and_invalid() {
  const auto profile = base_profile();
  const auto first = profile.cooldowns[0].id, second = profile.cooldowns[1].id;
  const auto never = [] { return SharedObservationChoice::cancel; };

  const auto bad = merge_calibration_result(std::nullopt, false, profile, first, "{",
                                            never);
  expect(as<VisualMergeRefusal>(bad) &&
             as<VisualMergeRefusal>(bad)->kind == VisualMergeRefusal::Kind::invalid_result,
         "an unreadable result changes nothing");
  const auto wrong = merge_calibration_result(
      std::nullopt, false, profile, second,
      calibration_json(profile, first, "shared", 1), never);
  expect(as<VisualMergeRefusal>(wrong) != nullptr,
         "a result for another cooldown is refused");

  auto merged = merge_calibration_result(std::nullopt, false, profile, first,
                                         calibration_json(profile, first, "shared", 1),
                                         never);
  auto *visual = std::get_if<VisualProfile>(&merged);
  if (!expect(visual && visual->observations.size() == 1 &&
                  visual->triggers[0].observation_id == "shared" &&
                  visual->profile_id == profile.id,
              "the first calibration creates the module"))
    return;
  // Share the observation with the second cooldown.
  auto shared_trigger = visual->triggers[0];
  shared_trigger.cooldown_id = second;
  visual->triggers.push_back(shared_trigger);

  const auto cancelled = merge_calibration_result(
      *visual, false, profile, first, calibration_json(profile, first, "x", 2), never);
  expect(as<VisualMergeRefusal>(cancelled) &&
             as<VisualMergeRefusal>(cancelled)->kind == VisualMergeRefusal::Kind::cancelled,
         "cancelling the shared choice keeps everything");

  const auto updated = merge_calibration_result(
      *visual, false, profile, first, calibration_json(profile, first, "x", 2),
      [] { return SharedObservationChoice::update_shared; });
  const auto *shared = as<VisualProfile>(updated);
  expect(shared && shared->observations.size() == 1 &&
             layerc::core::observation_id(shared->observations[0]) == "shared" &&
             std::get<AverageColorPredicate>(
                 std::get<PixelRule>(shared->observations[0]).predicate)
                     .target_color == 2,
         "updating keeps the shared id in place");

  const auto cloned = merge_calibration_result(
      *visual, false, profile, first, calibration_json(profile, first, "shared", 3),
      [] { return SharedObservationChoice::clone; });
  const auto *clone = as<VisualProfile>(cloned);
  expect(clone && clone->observations.size() == 2 &&
             clone->triggers[0].observation_id == "shared-2" &&
             clone->triggers[1].observation_id == "shared",
         "cloning gives this cooldown a fresh observation");

  const auto repaired = merge_calibration_result(
      *visual, true, profile, second, calibration_json(profile, second, "fresh", 4),
      never);
  const auto *replacement = as<VisualProfile>(repaired);
  expect(replacement && replacement->triggers.size() == 1 &&
             replacement->observations.size() == 1,
         "an invalid sidecar is replaced by the calibration");
}

void merge_limits() {
  const auto profile = base_profile();
  VisualProfile full;
  full.profile_id = profile.id;
  for (std::size_t index = 0; index < max_pixel_rules; ++index)
    full.observations.push_back(PixelRule{.id = "rule-" + std::to_string(index)});
  const auto refused = merge_calibration_result(
      full, false, profile, profile.cooldowns[0].id,
      calibration_json(profile, profile.cooldowns[0].id, "new", 5),
      [] { return SharedObservationChoice::cancel; });
  expect(as<VisualMergeRefusal>(refused) &&
             as<VisualMergeRefusal>(refused)->kind == VisualMergeRefusal::Kind::limit &&
             as<VisualMergeRefusal>(refused)->message ==
                 "This profile already has the maximum 4 pixel observations.",
         "the pixel observation limit is enforced");
  expect(observation_capacity_error(full, PixelRule{}, "rule-0") == std::nullopt,
         "replacing an observation of the same kind fits");
  expect(unique_observation_id("rule-1", full) == "rule-1-2" &&
             unique_observation_id("rule-1", full, "rule-1") == "rule-1" &&
             unique_observation_id("", full) == "visual-observation",
         "observation ids stay unique");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(model_seeds_and_builds),
                 LAYERC_TEST_CASE(model_reopens_existing_assignment),
                 LAYERC_TEST_CASE(template_selection_and_mask),
                 LAYERC_TEST_CASE(template_reopen_and_asset_validation),
                 LAYERC_TEST_CASE(merge_new_shared_and_invalid),
                 LAYERC_TEST_CASE(merge_limits))
