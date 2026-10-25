#pragma once

#include "layerc/core/profile.hpp"

#include <span>
#include <string>
#include <string_view>

namespace layerc::editor {

// Ends `first` with '.' unless it already ends in '.', '!', '?' or is empty,
// then appends ' ' + second. Callers use it so two diagnostic sentences never
// run together.
[[nodiscard]] std::string join_sentences(std::string first,
                                         std::string_view second);

// A cooldown-group validation error in dialog words: the group label
// `Group "<name>" (<id or 'row N+1'>)`, the field by its dialog label
// (id->ID, name->Name, cooldowns->Members with the member id when the path
// names one, durationMs->Duration, maxCharges->Charges, recovery->Recovery,
// retrigger->Retrigger, mode->Mode, unknown keys verbatim) and the validator
// message without the JSON path. Paths that do not address one group yield the
// message alone.
[[nodiscard]] std::string
cooldown_group_error_text(const layerc::core::ProfileError &error,
                          std::span<const layerc::core::CooldownGroup> groups);

} // namespace layerc::editor
