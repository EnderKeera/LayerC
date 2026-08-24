#pragma once

#include <span>
#include <vector>

namespace layerc::daemon {

template <typename SessionT>
[[nodiscard]] std::vector<SessionT *>
select_companion_sessions(const std::span<SessionT *const> foreground,
                          const std::span<SessionT *const> all_sessions,
                          const bool gamebar_foreground) {
  if (!gamebar_foreground)
    return {foreground.begin(), foreground.end()};

  std::vector<SessionT *> selected;
  selected.reserve(all_sessions.size());
  for (auto *session : all_sessions)
    if (session != nullptr && !session->is_global())
      selected.push_back(session);
  if (!selected.empty())
    return selected;

  for (auto *session : all_sessions)
    if (session != nullptr && session->is_global())
      selected.push_back(session);
  return selected;
}

} // namespace layerc::daemon
