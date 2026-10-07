#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor {

// The profile a new editor draft starts from.
[[nodiscard]] layerc::core::GameProfile make_new_profile();

// "name — app.exe (off)" in UTF-8: the list text every frontend shows.
[[nodiscard]] std::string
profile_list_text(const layerc::core::GameProfile &profile);

// Smallest "<base>" then "<base>-<n>" id unused by the predicate.
template <typename IsUsed>
[[nodiscard]] std::string next_unique_id(const std::string_view base,
                                         IsUsed &&is_used) {
  std::string candidate(base);
  for (std::size_t suffix = 2; is_used(candidate); ++suffix)
    candidate = std::string(base) + "-" + std::to_string(suffix);
  return candidate;
}

[[nodiscard]] std::string
unique_panel_id(std::string_view base,
                const std::vector<layerc::core::Panel> &existing,
                const std::vector<layerc::core::Panel> *generated = nullptr);

// Parses a positive decimal seconds value ("1.5") within the cooldown
// duration limit, rounded to whole milliseconds.
[[nodiscard]] std::optional<layerc::core::Milliseconds>
parse_duration_seconds(std::string_view seconds);

// "path: message", or the message alone for a document-level error.
[[nodiscard]] std::string
profile_error_line(const layerc::core::ProfileError &error);

// Drops the cooldown's visual trigger and its observation once unreferenced.
void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             std::string_view cooldown_id);
void gc_orphan_visual_observation(layerc::core::VisualProfile &visual,
                                  std::string_view observation_id);

} // namespace layerc::editor
