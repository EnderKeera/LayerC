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
// - `global` profiles are a policy case selected by GlobalMatchPolicy: they
//   either follow the active window or are bound to no window.
//
// resolve_game_match is the only exported entry point; both platform
// adapters call it, so ranking never bypasses the conjunctive rule.

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

enum class GlobalMatchPolicy {
  // A session target: a global profile follows the active candidate, else
  // the first one.
  follow_active,
  // A window-binding query: a global profile is bound to no window.
  not_window_bound,
};

struct GameMatchSelection {
  bool exact_path{};
  // Index of the window a session already follows. Among equally active
  // matches it wins, so focus loss does not retarget a session.
  std::optional<std::size_t> preferred;
  GlobalMatchPolicy global{GlobalMatchPolicy::follow_active};
};

// The GameMatch matcher. Only candidates for which every specified field
// holds are eligible; among them the active window wins, then the
// preferred candidate, then poll order. Asking whether one window matches
// is a one-candidate call. Nullopt when nothing is eligible.
[[nodiscard]] std::optional<std::size_t>
resolve_game_match(const std::vector<MatchCandidate> &candidates,
                   const GameMatch &match, const GameMatchSelection &selection);

} // namespace layerc::core
