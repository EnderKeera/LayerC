#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::editor {

// The selected cooldown's visual trigger, or nullptr.
[[nodiscard]] layerc::core::VisualTrigger *
find_visual_trigger(layerc::core::VisualProfile &visual,
                    std::string_view cooldown_id) noexcept;
[[nodiscard]] const layerc::core::VisualTrigger *
find_visual_trigger(const layerc::core::VisualProfile &visual,
                    std::string_view cooldown_id) noexcept;

// "<requested>" or "<requested>-<n>" unused by the profile's observations;
// `allowed` is treated as free (the id being replaced).
[[nodiscard]] std::string
unique_observation_id(std::string_view requested,
                      const layerc::core::VisualProfile &profile,
                      std::string_view allowed = {});

// The limit message when one more observation of the candidate's kind does
// not fit; `replacing_id` names an observation the candidate replaces.
[[nodiscard]] std::optional<std::string>
observation_capacity_error(const layerc::core::VisualProfile &profile,
                           const layerc::core::VisualObservation &candidate,
                           std::string_view replacing_id = {});

enum class SharedObservationChoice { update_shared, clone, cancel };

struct VisualMergeRefusal {
  enum class Kind { invalid_result, limit, invalid_profile, cancelled };
  Kind kind{Kind::invalid_result};
  std::string message;
};
using VisualMergeResult =
    std::variant<layerc::core::VisualProfile, VisualMergeRefusal>;

// Merges one calibration result (a one-observation, one-trigger visual
// document for `cooldown_id`) into the open document's visual module. An
// invalid current sidecar is replaced. A recalibrated observation shared by
// other cooldowns asks `choose_shared` whether to update it or clone it.
// The returned module is complete and validated against `profile`.
[[nodiscard]] VisualMergeResult merge_calibration_result(
    const std::optional<layerc::core::VisualProfile> &current,
    bool current_invalid, const layerc::core::GameProfile &profile,
    std::string_view cooldown_id, const layerc::core::VisualProfile &calibration,
    const std::function<SharedObservationChoice()> &choose_shared);

} // namespace layerc::editor
