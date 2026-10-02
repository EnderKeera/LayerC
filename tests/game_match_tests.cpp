#include "core_test_cases.hpp"
#include "layerc/core/game_match.hpp"
#include "test_support.hpp"

#include <optional>
#include <string>
#include <vector>

namespace layerc::test {
namespace {

using layerc::core::GameMatch;
using layerc::core::MatchCandidate;

struct MatchCase {
  const char *name;
  GameMatch match;
  MatchCandidate candidate;
  bool exact_path;
  bool expected;
};

// Shared single-policy table: both platform adapters call this matcher, so
// these rows define Windows and Linux matching alike.
[[nodiscard]] std::vector<MatchCase> match_cases() {
  const MatchCandidate game{.executable_name = "game.exe",
                            .executable_path = "/usr/bin/game.exe",
                            .class_name = "steam_app",
                            .title = "Some Game"};
  return {
      {"name equality folds case", {.executable_name = "GAME.EXE"}, game,
       true, true},
      {"name is equality, not substring", {.executable_name = "game"}, game,
       true, false},
      {"exact path keeps case distinct",
       {.executable_path = "/USR/BIN/GAME.EXE"}, game, true, false},
      {"folded path ignores case", {.executable_path = "/USR/BIN/GAME.EXE"},
       game, false, true},
      {"different path never matches", {.executable_path = "/usr/bin/other"},
       game, false, false},
      {"class is a folded substring", {.window_class = "Steam"}, game, true,
       true},
      {"title is a folded substring", {.window_title = "some game"}, game,
       true, true},
      {"every specified field must hold",
       {.executable_name = "game.exe", .window_title = "Other"}, game, true,
       false},
      {"name plus title both holding matches",
       {.executable_name = "game.exe", .window_title = "Game"}, game, true,
       true},
      {"a missing candidate name does not fall back to the title",
       {.executable_name = "game.exe", .window_title = "Some Game"},
       {.class_name = "steam_app", .title = "Some Game"}, true, false},
      {"title-only matching needs no executable path",
       {.window_title = "Game"}, {.title = "Some Game"}, true, true},
      {"no identity field never matches", {}, game, true, false},
      {"global profiles are not window-bound", {.global = true}, game, true,
       false},
      {"undecodable matcher fails closed",
       {.executable_name = std::string(1, '\xFF'), .window_title = "Game"},
       game, false, false},
      {"undecodable candidate field fails closed", {.window_title = "Game"},
       {.class_name = std::string(1, '\xFF'), .title = "Some Game"}, false,
       false},
      {"byte-exact paths still fail closed on invalid UTF-8",
       {.window_title = "Game"},
       {.executable_path = std::string(1, '\xFF'), .title = "Some Game"},
       true, false},
      {"non-ASCII fields share the UTF-8 fold", {.window_class = "\xC3\x89"},
       {.class_name = "class-\xC3\xA9"}, true, true},
  };
}

} // namespace

void test_game_match_policy_table() {
  for (const auto &row : match_cases()) {
    expect(layerc::core::candidate_matches_game(row.candidate, row.match,
                                                row.exact_path) == row.expected,
           std::string("GameMatch: ") + row.name);
    // Selection never widens the matcher: a lone candidate is selected
    // exactly when it matches.
    if (!row.match.global) {
      const auto selected = layerc::core::select_game_candidate(
          {row.candidate}, row.match, {.exact_path = row.exact_path});
      expect(selected.has_value() == row.expected,
             std::string("GameMatch selection agrees: ") + row.name);
    }
  }
}

void test_game_match_selection() {
  using layerc::core::select_game_candidate;
  GameMatch match{.executable_name = "game.exe"};
  std::vector<MatchCandidate> windows{
      {.executable_name = "game.exe", .title = "first"},
      {.executable_name = "other.exe", .title = "game.exe", .active = true},
      {.executable_name = "GAME.EXE", .title = "third"},
  };
  expect(select_game_candidate(windows, match, {}) == 0,
         "selection keeps poll order among background matches");
  expect(select_game_candidate(windows, match, {.preferred = 2}) == 2,
         "a previous target wins background ties");
  expect(select_game_candidate(windows, match, {.preferred = 1}) == 0,
         "an unmatched previous target preserves normal order");
  windows[2].active = true;
  expect(select_game_candidate(windows, match, {.preferred = 0}) == 2,
         "the active match wins over a previous background target");
  expect(!select_game_candidate({}, match, {}).has_value(),
         "no windows selects nothing");

  const GameMatch global{.global = true};
  expect(select_game_candidate(windows, global, {}) == 1,
         "global profiles follow the first active window");
  for (auto &window : windows) window.active = false;
  expect(select_game_candidate(windows, global, {}) == 0,
         "global profiles fall back to the first window");
}

} // namespace layerc::test
