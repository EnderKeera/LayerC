#pragma once

#include <algorithm>
#include <cstdint>

namespace layerc::editor {

// What a status message reports (DesignSystem.md, StatusText): assigned where
// the outcome is known and carried beside the message, never read back from
// its text. Each renderer maps it to its drawn glyph; whether a screen reader
// hears the message is StatusVoice's separate decision. Declared in rising
// severity.
enum class StatusKind : std::uint8_t {
  none,        // no message, no glyph
  information, // neither good nor bad: progress, notes, no-ops, cancellations
  success,     // worked or healthy
  warning,     // works but needs a look
  failure,     // failed, refused or stopped
};

// One line reporting two states shows the more severe.
[[nodiscard]] constexpr StatusKind most_severe(const StatusKind first,
                                               const StatusKind second) noexcept {
  return std::max(first, second);
}

} // namespace layerc::editor
