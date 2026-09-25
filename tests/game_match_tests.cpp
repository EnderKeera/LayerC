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

} // namespace layerc::test
