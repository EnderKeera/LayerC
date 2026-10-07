// Canonical GameMatch resolution policy shared by the Windows and Linux
// window adapters. See game_match.hpp for the comparison rules. The match
// side is folded once per call and each candidate once; ranking only
// orders candidates the conjunctive rule accepts.

#include "layerc/core/game_match.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>

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

[[nodiscard]] bool has_identity(const FoldedSide &match) noexcept {
  return match.name.has_value() || match.path.has_value() ||
         match.clazz.has_value() || match.title.has_value();
}

[[nodiscard]] bool folded_matches(const FoldedSide &candidate,
                                  const FoldedSide &match) {
  // An empty matcher field is vacuous; a specified one must hold. No
  // executable-path presence guard: a pure title/class match must not
  // depend on the candidate carrying a path.
  return candidate.usable && field_equals(candidate.name, match.name) &&
         field_equals(candidate.path, match.path) &&
         field_contains(candidate.clazz, match.clazz) &&
         field_contains(candidate.title, match.title);
}

} // namespace

std::optional<std::size_t>
resolve_game_match(const std::vector<MatchCandidate> &candidates,
                   const GameMatch &match, const GameMatchSelection &selection) {
  if (candidates.empty()) return std::nullopt;
  if (match.global) {
    if (selection.global == GlobalMatchPolicy::not_window_bound)
      return std::nullopt;
    const auto active = std::ranges::find_if(
        candidates, [](const MatchCandidate &candidate) { return candidate.active; });
    return static_cast<std::size_t>(
        active == candidates.end() ? 0 : active - candidates.begin());
  }
  const FoldedSide folded_match = fold_match(match, selection.exact_path);
  if (!folded_match.usable || !has_identity(folded_match)) return std::nullopt;
  // Rank (inactive, not preferred); ties keep the earlier candidate so poll
  // order stays deterministic.
  std::optional<std::size_t> best;
  int best_rank = 4;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!folded_matches(fold_candidate(candidates[index], selection.exact_path),
                        folded_match))
      continue;
    const int rank = (candidates[index].active ? 0 : 2) +
                     (selection.preferred == index ? 0 : 1);
    if (rank < best_rank) {
      best = index;
      best_rank = rank;
    }
  }
  return best;
}

} // namespace layerc::core
