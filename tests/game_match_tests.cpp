#include "core_test_cases.hpp"
#include "layerc/core/game_match.hpp"
#include "test_support.hpp"

#include <string>
#include <vector>

namespace layerc::test {

void test_game_match_resolution_policy() {
  using layerc::core::GameMatch;
  using layerc::core::MatchCandidate;

  const std::vector<MatchCandidate> windows{
      {.executable_name = "game.exe",
       .executable_path = "/usr/bin/game.exe",
       .class_name = "steam_app",
       .title = "Some Game",
       .active = false},
      {.executable_name = "game.exe",
       .executable_path = "/usr/bin/game.exe",
       .class_name = "Unrelated",
       .title = "Idle",
       .active = true},
  };

  GameMatch by_name;
  by_name.executable_name = "GAME.EXE";
  const auto name_hit =
      layerc::core::resolve_game_match(windows, by_name, true);
  expect(name_hit.has_value() && *name_hit == 1,
         "name matching prefers the active window and folds case");

  GameMatch by_path;
  by_path.executable_path = "/usr/bin/other.bin";
  expect(!layerc::core::resolve_game_match(windows, by_path, true).has_value(),
         "exact path opt-in does not match a different binary");

  GameMatch by_class;
  by_class.window_class = "Steam";
  const auto class_hit =
      layerc::core::resolve_game_match(windows, by_class, true);
  expect(class_hit.has_value() && *class_hit == 0,
         "class match is a folded substring");

  GameMatch by_title;
  by_title.window_title = "some game";
  const auto title_hit =
      layerc::core::resolve_game_match(windows, by_title, true);
  expect(title_hit.has_value() && *title_hit == 0,
         "title match is a folded substring");

  GameMatch no_identity;
  expect(
      !layerc::core::resolve_game_match(windows, no_identity, true).has_value(),
      "a profile without identity fields never matches");

  GameMatch invalid;
  invalid.executable_name = std::string(1, '\xFF');
  invalid.window_title = "Idle";
  expect(!layerc::core::candidate_matches_game(windows[1], invalid, true) &&
             !layerc::core::resolve_game_match(windows, invalid, true)
                  .has_value(),
         "an undecodable matcher field rejects instead of weakening to "
         "title-only matching");

  // Single-candidate conjunctive rule (Windows adapter semantics): every
  // specified field must hold, and the path folds on case-insensitive
  // filesystems.
  GameMatch conjunctive;
  conjunctive.executable_name = "game.exe";
  conjunctive.executable_path = "/USR/BIN/GAME.EXE";
  expect(layerc::core::candidate_matches_game(windows[0], conjunctive, false),
         "folded path comparison matches across case differences");
  expect(!layerc::core::candidate_matches_game(windows[0], conjunctive, true),
         "exact path comparison keeps case differences distinct");

  GameMatch global;
  global.global = true;
  expect(!layerc::core::resolve_game_match(windows, global, true).has_value(),
         "global profiles never resolve through window matching");
}

void test_game_match_identity_fallback() {
  using namespace layerc::core;
  GameMatch match;
  match.executable_name = "missing.exe";
  match.executable_path = "/games/game.exe";
  match.window_class = "Class";
  match.window_title = "Game";
  std::vector<MatchCandidate> windows{
      {.class_name = "GameClass", .title = "Game", .active = false},
      {.class_name = "GAMECLASS", .title = "Game", .active = true}};
  expect(resolve_game_match(windows, match, true) == 1 &&
             !candidate_matches_game(windows[1], match, true),
         "CM-05: a missing executable identity falls back to class, not conjunction");
  windows.push_back({.executable_name = "other.exe",
                     .executable_path = "/games/game.exe",
                     .active = false});
  expect(resolve_game_match(windows, match, true) == 2,
         "CM-05: an inactive successful path outranks an active class");
  windows.push_back({.executable_name = "MISSING.EXE", .active = false});
  expect(resolve_game_match(windows, match, true) == 3,
         "CM-05: a successful name outranks all lower fallback identities");
  windows.push_back({.executable_name = "missing.exe", .active = true});
  windows.push_back({.executable_name = "missing.exe", .active = true});
  expect(resolve_game_match(windows, match, true) == 4,
         "CM-05: activity breaks same-kind ties and poll order breaks equal ties");

  windows = {{.executable_name = "other.exe",
              .executable_path = "/GAMES/GAME.EXE",
              .class_name = "other", .title = "A Game", .active = true},
             {.class_name = "GameClass", .active = false}};
  expect(resolve_game_match(windows, match, true) == 1 &&
             resolve_game_match(windows, match, false) == 0,
         "CM-05: exact path mismatches fall through; folded path successes rank higher");
  windows.resize(1);
  expect(resolve_game_match(windows, match, true) == 0,
         "CM-05: unsuccessful name, path and class still permit a title identity");

  for (const bool exact_path : {false, true}) {
    auto invalid = match;
    invalid.executable_name = std::string(1, '\xFF');
    expect(!resolve_game_match(windows, invalid, exact_path) &&
               !candidate_matches_game(windows.front(), invalid, exact_path),
           "CM-05: invalid matcher UTF-8 must not weaken to a fallback");
    auto invalid_windows = windows;
    invalid_windows.front().class_name = std::string(1, '\xFF');
    expect(!resolve_game_match(invalid_windows, match, exact_path),
           "CM-05: invalid candidate UTF-8 must not weaken to a title fallback");
    invalid_windows = windows;
    invalid_windows.front().executable_path = std::string(1, '\xFF');
    expect(!resolve_game_match(invalid_windows, match, exact_path),
           "CM-05: byte-exact paths still fail closed on invalid UTF-8");
  }
  match = {};
  match.executable_name = "unavailable";
  match.window_class = "\xC3\x89";
  windows = {{.class_name = "class-\xC3\xA9", .active = true}};
  expect(resolve_game_match(windows, match, true) == 0,
         "CM-05: lower-priority identities keep the shared UTF-8 comparison fold");
}

} // namespace layerc::test
