#pragma once

// Single owner for GameMatch resolution policy (executable-name-first,
// exact-path as Advanced opt-in). Platform adapters enumerate windows their
// own way but fill MatchCandidate and share these rules, so matching cannot
// drift per platform.
//
// Comparison rules (all through the shared try_casefold_utf8 fold):
// - executable_name: folded equality.
// - executable_path: byte-exact when `exact_path` is true (case-sensitive
//   filesystems), shared fold otherwise (case-insensitive filesystems).
// - window_class / window_title: folded substring.
// - Any specified but undecodable matcher field fails closed: the match is
//   rejected rather than weakening to the remaining fields.

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

// False when any specified matcher field is not valid UTF-8.
[[nodiscard]] bool game_match_usable(const GameMatch &match);

// matches_all: conjunctive single-candidate filter — every specified field
// must hold; an empty matcher field is vacuous. A pure title/class match
// does not require the candidate to carry an executable path. Used by
// window enumeration to test one window at a time.
[[nodiscard]] bool candidate_matches_game(const MatchCandidate &candidate,
                                          const GameMatch &match,
                                          bool exact_path);

// best_identity: disjunctive cascade over a poll with identity precedence
// (name → path → class → title), preferring the active window within each
// kind. Unlike matches_all, only the winning identity kind participates —
// a candidate matching on title alone wins when no candidate matches on
// name/path/class. Nullopt when no candidate matches, no identity field is
// specified, or the matcher fields are unusable. Used by trackers polling
// many windows at once.
[[nodiscard]] std::optional<std::size_t>
resolve_game_match(const std::vector<MatchCandidate> &candidates,
                   const GameMatch &match, bool exact_path);

} // namespace layerc::core
