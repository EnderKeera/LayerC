#pragma once

// Single owner for GameMatch resolution policy. Platform adapters enumerate
// windows their own way but fill MatchCandidate and share this one
// conjunctive rule, so a portable profile matches the same windows on every
// platform.
//
// Comparison rules (all through the shared try_casefold_utf8 fold):
// - executable_name: folded equality.
// - executable_path: byte-exact when `exact_path` is true (case-sensitive
//   filesystems), shared fold otherwise (case-insensitive filesystems).
// - window_class / window_title: folded substring.
// - Every specified matcher field must hold; an empty field is vacuous and
//   a matcher with no identity field never matches.
// - Any specified but undecodable matcher or candidate field fails closed:
//   the match is rejected rather than weakening to the remaining fields.
// - `global` profiles are not window-bound: candidate_matches_game rejects
//   them, and select_game_candidate follows the active window.

#include "layerc/core/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace layerc::core {

struct MatchCandidate {
  std::string executable_name;
  std::string executable_path;
  std::string class_name;
  std::string title;
  bool active{};
};

// The matcher: true when every specified field holds for `candidate`.
[[nodiscard]] bool candidate_matches_game(const MatchCandidate &candidate,
                                          const GameMatch &match,
                                          bool exact_path);

struct GameMatchSelection {
  bool exact_path{};
  // Index of the window a session already follows. Among equally active
  // matches it wins, so focus loss does not retarget a session.
  std::optional<std::size_t> preferred;
};

// Picks one window for a single-target session. Only candidates accepted by
// candidate_matches_game are eligible; the active window wins, then the
// preferred candidate, then poll order. Global profiles pick the active
// candidate, else the first. Nullopt when nothing is eligible.
[[nodiscard]] std::optional<std::size_t>
select_game_candidate(const std::vector<MatchCandidate> &candidates,
                      const GameMatch &match,
                      const GameMatchSelection &selection);

} // namespace layerc::core
