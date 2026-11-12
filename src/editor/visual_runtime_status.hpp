#pragma once

#include "status_kind.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace layerc::editor {

// The kind of LayerD's visual recognition message. LayerD sends only the
// text (format_visual_status in src/daemon/daemon_visual.cpp), so this is the
// one bounded place that reads its known phrases: the exact state after the
// prefix, with an optional " - <GPU>" suffix ignored. Anything unrecognized is
// information, never success.
[[nodiscard]] inline StatusKind
visual_runtime_status_kind(const std::wstring_view text) noexcept {
  constexpr std::wstring_view prefix = L"Visual recognition: ";
  if (!text.starts_with(prefix))
    return StatusKind::information;
  const auto remainder = text.substr(prefix.size());
  const auto state = remainder.substr(0, remainder.find(L" - "));
  constexpr std::array<std::pair<std::wstring_view, StatusKind>, 10> phrases{{
      {L"WGC active", StatusKind::success},
      {L"Desktop Duplication fallback", StatusKind::warning},
      {L"inactive", StatusKind::information},
      {L"disabled", StatusKind::information},
      {L"connecting", StatusKind::information},
      {L"blocked (invalid sidecar)", StatusKind::failure},
      {L"isolated helper quarantined", StatusKind::failure},
      {L"isolated helper quarantined (use Runtime Diagnostics to retry)",
       StatusKind::failure},
      {L"template asset missing or corrupt", StatusKind::failure},
      {L"capture unavailable", StatusKind::failure},
  }};
  for (const auto &[phrase, kind] : phrases) {
    if (state == phrase)
      return kind;
  }
  return StatusKind::information;
}

} // namespace layerc::editor
