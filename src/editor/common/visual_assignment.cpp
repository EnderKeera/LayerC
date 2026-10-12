#include "visual_assignment.hpp"

#include "profile_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <utility>

namespace layerc::editor {
namespace {

using layerc::core::VisualObservation;
using layerc::core::VisualProfile;

void set_observation_id(VisualObservation &observation,
                        const std::string_view id) {
  std::visit([&](auto &item) { item.id = id; }, observation);
}

// Recalibration keeps the observation's authored position.
void replace_observation(VisualProfile &profile, const std::string_view old_id,
                         const VisualObservation &observation) {
  const auto found = std::ranges::find_if(
      profile.observations, [&](const VisualObservation &item) {
        return layerc::core::observation_id(item) == old_id;
      });
  if (found == profile.observations.end())
    profile.observations.push_back(observation);
  else
    *found = observation;
}

// Fresh observation id, append, and trigger link for a calibration result
// that has no shared prior observation to reuse.
std::optional<VisualMergeRefusal>
install_new_observation(VisualProfile &profile, VisualObservation &observation,
                        layerc::core::VisualTrigger &trigger) {
  if (auto limit = observation_capacity_error(profile, observation))
    return VisualMergeRefusal{VisualMergeRefusal::Kind::limit, std::move(*limit)};
  const auto id =
      unique_observation_id(layerc::core::observation_id(observation), profile);
  set_observation_id(observation, id);
  trigger.observation_id = id;
  profile.observations.push_back(observation);
  return std::nullopt;
}

} // namespace

layerc::core::VisualTrigger *
find_visual_trigger(VisualProfile &visual,
                    const std::string_view cooldown_id) noexcept {
  const auto found = std::ranges::find(visual.triggers, cooldown_id,
                                      &layerc::core::VisualTrigger::cooldown_id);
  return found == visual.triggers.end() ? nullptr : &*found;
}

const layerc::core::VisualTrigger *
find_visual_trigger(const VisualProfile &visual,
                    const std::string_view cooldown_id) noexcept {
  return find_visual_trigger(const_cast<VisualProfile &>(visual), cooldown_id);
}

std::string unique_observation_id(const std::string_view requested,
                                  const VisualProfile &profile,
                                  const std::string_view allowed) {
  const std::string base =
      requested.empty() ? "visual-observation" : std::string(requested);
  return next_unique_id(base, [&](const std::string_view value) {
    if (!allowed.empty() && value == allowed) return false;
    return layerc::core::find_observation(profile, value) != nullptr;
  });
}

std::optional<std::string>
observation_capacity_error(const VisualProfile &profile,
                           const VisualObservation &candidate,
                           const std::string_view replacing_id) {
  const bool is_pixel = std::holds_alternative<layerc::core::PixelRule>(candidate);
  const auto same_kind = [&](const VisualObservation &observation) {
    return observation.index() == candidate.index();
  };
  auto count = static_cast<std::size_t>(
      std::ranges::count_if(profile.observations, same_kind));
  if (!replacing_id.empty()) {
    const auto *replaced = layerc::core::find_observation(profile, replacing_id);
    if (replaced != nullptr && same_kind(*replaced)) --count;
  }
  const auto maximum = is_pixel ? layerc::core::max_pixel_rules
                                : layerc::core::max_template_states;
  if (count < maximum) return std::nullopt;
  return "This profile already has the maximum " + std::to_string(maximum) +
         (is_pixel ? " pixel" : " template") + " observations.";
}

VisualMergeResult merge_calibration_result(
    const std::optional<VisualProfile> &current, const bool current_invalid,
    const layerc::core::GameProfile &profile, const std::string_view cooldown_id,
    const std::string_view visual_json,
    const std::function<SharedObservationChoice()> &choose_shared) {
  const auto parsed = layerc::core::parse_visual_profile_json(visual_json, profile);
  const auto *candidate_profile = std::get_if<VisualProfile>(&parsed);
  if (candidate_profile == nullptr || cooldown_id.empty() ||
      candidate_profile->observations.size() != 1 ||
      candidate_profile->triggers.size() != 1 ||
      candidate_profile->triggers.front().cooldown_id != cooldown_id)
    return VisualMergeRefusal{
        VisualMergeRefusal::Kind::invalid_result,
        "LayerC Calibration returned an invalid result. No profile data was "
        "changed."};

  VisualProfile merged = current_invalid ? VisualProfile{}
                                         : current.value_or(VisualProfile{});
  merged.profile_id = profile.id;
  merged.sample_rate_hz = candidate_profile->sample_rate_hz;
  merged.quality = candidate_profile->quality;
  auto candidate_observation = candidate_profile->observations.front();
  auto candidate_trigger = candidate_profile->triggers.front();
  auto *old_trigger = find_visual_trigger(merged, cooldown_id);
  std::string old_observation_id;
  if (old_trigger != nullptr) {
    old_observation_id = old_trigger->observation_id;
    const auto references = std::ranges::count(
        merged.triggers, old_observation_id,
        &layerc::core::VisualTrigger::observation_id);
    bool update_shared = references <= 1;
    if (references > 1) {
      const auto choice = choose_shared ? choose_shared()
                                        : SharedObservationChoice::cancel;
      if (choice == SharedObservationChoice::cancel)
        return VisualMergeRefusal{VisualMergeRefusal::Kind::cancelled,
                                  "Visual calibration was not applied."};
      update_shared = choice == SharedObservationChoice::update_shared;
    }
    if (update_shared) {
      if (auto limit = observation_capacity_error(merged, candidate_observation,
                                                  old_observation_id))
        return VisualMergeRefusal{VisualMergeRefusal::Kind::limit,
                                  std::move(*limit)};
      set_observation_id(candidate_observation, old_observation_id);
      replace_observation(merged, old_observation_id, candidate_observation);
      candidate_trigger.observation_id = old_observation_id;
    } else if (auto refusal = install_new_observation(
                   merged, candidate_observation, candidate_trigger)) {
      return std::move(*refusal);
    }
    *old_trigger = candidate_trigger;
  } else {
    if (auto refusal = install_new_observation(merged, candidate_observation,
                                               candidate_trigger))
      return std::move(*refusal);
    merged.triggers.push_back(candidate_trigger);
  }

  gc_orphan_visual_observation(merged, old_observation_id);
  const auto error = layerc::core::validate_visual_profile(merged, profile);
  if (!error.message.empty())
    return VisualMergeRefusal{VisualMergeRefusal::Kind::invalid_profile,
                              error.message};
  return merged;
}

} // namespace layerc::editor
