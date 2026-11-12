#pragma once

#include <cstdint>
#include <string_view>

namespace layerc::editor {

// How a status line message reaches a screen reader. `announced` is the
// outcome of an explicit action or a failure. `changed` is a failure that
// repeats while its cause stays, such as autosave after every edit: it is
// spoken once, then again only after a different announcement. `silent`
// follows every edit or reports background progress.
enum class StatusVoice : std::uint8_t { announced, changed, silent };

namespace detail {
// One rule for both spellings: the Windows editor keeps wide text, the Linux
// editor narrow UTF-8 (DesignSystem.md, Screen readers).
template <typename Text>
[[nodiscard]] bool status_is_announced(const Text last_announced,
                                       const Text text,
                                       const StatusVoice voice) noexcept {
  if (text.empty() || voice == StatusVoice::silent) return false;
  return voice == StatusVoice::announced || text != last_announced;
}
} // namespace detail

// Whether `text` is spoken, given the last message that was.
[[nodiscard]] inline bool
status_is_announced(const std::string_view last_announced,
                    const std::string_view text,
                    const StatusVoice voice) noexcept {
  return detail::status_is_announced(last_announced, text, voice);
}
[[nodiscard]] inline bool
status_is_announced(const std::wstring_view last_announced,
                    const std::wstring_view text,
                    const StatusVoice voice) noexcept {
  return detail::status_is_announced(last_announced, text, voice);
}

} // namespace layerc::editor
