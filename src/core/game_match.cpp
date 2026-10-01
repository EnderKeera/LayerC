// Canonical GameMatch resolution policy shared by the Windows and Linux
// window adapters. See game_match.hpp for the comparison rules.
//
// Two named semantics share one folded model:
// - matches_all (candidate_matches_game): conjunctive single-candidate
//   filter — every specified field must hold. Used by window enumeration
//   to test one window at a time.
// - best_identity (resolve_game_match): disjunctive cascade over a poll —
//   identity precedence name → path → class → title, preferring the active
//   window within each kind. Used by trackers polling many windows.
// The match side is folded once; each candidate is folded once per call;
// resolve scores every candidate once as (level, !active) and picks the
// minimum instead of scanning per level.

#include "layerc/core/game_match.hpp"

#include "layerc/core/utf8.hpp"

namespace layerc::core {
namespace {

// One pre-folded side of a comparison. `specified` marks a matcher field
// the profile actually sets; `text` holds the folded (or byte-exact path)
// form. An undecodable specified field poisons the whole side via `usable`.
struct FoldedSide {
  bool usable{true};
  std::optional<std::string> name;
  std::optional<std::string> path;
  std::optional<std::string> clazz;
  std::optional<std::string> title;
};

[[nodiscard]] std::optional<std::string> fold_field(std::string_view text) {
  return try_casefold_utf8(text);
}

void fold_into(std::optional<std::string> &slot, std::string_view text,
               bool &usable) {
  if (text.empty()) return;
  auto folded = fold_field(text);
  if (!folded.has_value()) {
    usable = false;
    return;
  }
  slot = std::move(*folded);
}

[[nodiscard]] FoldedSide fold_side(std::string_view name, std::string_view path,
                                   std::string_view clazz,
                                   std::string_view title, bool exact_path) {
  FoldedSide folded;
  fold_into(folded.name, name, folded.usable);
  if (!path.empty()) {
    if (exact_path) {
      folded.usable = folded.usable && is_valid_utf8(path);
      folded.path = std::string(path);
    } else {
      fold_into(folded.path, path, folded.usable);
    }
  }
  fold_into(folded.clazz, clazz, folded.usable);
  fold_into(folded.title, title, folded.usable);
  return folded;
}

[[nodiscard]] FoldedSide fold_match(const GameMatch &match, bool exact_path) {
  return fold_side(match.executable_name, match.executable_path,
                   match.window_class, match.window_title, exact_path);
}

[[nodiscard]] FoldedSide fold_candidate(const MatchCandidate &candidate,
                                        bool exact_path) {
  return fold_side(candidate.executable_name, candidate.executable_path,
                   candidate.class_name, candidate.title, exact_path);
}

[[nodiscard]] bool field_equals(const std::optional<std::string> &candidate,
                                const std::optional<std::string> &expected) {
  if (!expected.has_value()) return true;
  return candidate.has_value() && *candidate == *expected;
}

[[nodiscard]] bool field_contains(const std::optional<std::string> &candidate,
                                  const std::optional<std::string> &needle) {
  if (!needle.has_value()) return true;
  if (needle->empty() || !candidate.has_value()) return false;
  return candidate->find(*needle) != std::string::npos;
}

// Identity level of one folded candidate against the folded match, or -1.
// Levels are precedence-ordered (name → path → class → title); only
// specified matcher fields participate, so an empty match never matches.
[[nodiscard]] int identity_level(const FoldedSide &candidate,
                                 const FoldedSide &match) noexcept {
  if (match.name.has_value() && field_equals(candidate.name, match.name))
    return 0;
  if (match.path.has_value() && field_equals(candidate.path, match.path))
    return 1;
  if (match.clazz.has_value() && field_contains(candidate.clazz, match.clazz))
    return 2;
  if (match.title.has_value() && field_contains(candidate.title, match.title))
    return 3;
  return -1;
}

} // namespace

bool game_match_usable(const GameMatch &match) {
  for (const std::string_view field :
       {std::string_view(match.executable_name),
        std::string_view(match.executable_path),
        std::string_view(match.window_class),
        std::string_view(match.window_title)}) {
    if (!field.empty() && !fold_field(field).has_value()) return false;
  }
  return true;
}

bool candidate_matches_game(const MatchCandidate &candidate,
                            const GameMatch &match, const bool exact_path) {
  if (match.global || !game_match_usable(match)) return false;
  const FoldedSide folded_match = fold_match(match, exact_path);
  const FoldedSide folded_candidate = fold_candidate(candidate, exact_path);
  if (!folded_match.usable || !folded_candidate.usable) return false;
  // Conjunctive: an empty matcher field is vacuous; a specified one must
  // hold. No executable-path presence guard — a pure title/class match must
  // not depend on the candidate carrying a path.
  return field_equals(folded_candidate.name, folded_match.name) &&
         field_equals(folded_candidate.path, folded_match.path) &&
         field_contains(folded_candidate.clazz, folded_match.clazz) &&
         field_contains(folded_candidate.title, folded_match.title);
}

std::optional<std::size_t>
resolve_game_match(const std::vector<MatchCandidate> &candidates,
                   const GameMatch &match, const bool exact_path) {
  if (candidates.empty() || match.global || !game_match_usable(match))
    return std::nullopt;
  const FoldedSide folded_match = fold_match(match, exact_path);
  if (!folded_match.usable) return std::nullopt;
  // Single scored pass: (identity level, background penalty). Lower wins;
  // ties keep the earlier candidate, so poll order stays deterministic.
  std::optional<std::size_t> best;
  int best_level = 4;
  bool best_inactive = true;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const FoldedSide folded = fold_candidate(candidates[index], exact_path);
    if (!folded.usable) continue;
    const int level = identity_level(folded, folded_match);
    if (level < 0) continue;
    const bool inactive = !candidates[index].active;
    if (!best.has_value() || level < best_level ||
        (level == best_level && best_inactive && !inactive)) {
      best = index;
      best_level = level;
      best_inactive = inactive;
    }
  }
  return best;
}

} // namespace layerc::core
