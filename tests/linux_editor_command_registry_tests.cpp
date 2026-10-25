// The Linux editor command table: completeness, order, the menu/context truth
// tables and the derived help text.
#include "editor_commands.hpp"
#include "editor_app.hpp"
#include "editor_menus.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using layerc::editor::EditorApp;
using layerc::editor::EditorCommand;
using layerc::editor::EditorMenuContext;
using layerc::test::expect;

constexpr unsigned profiles = layerc::editor::context_bit(EditorMenuContext::profiles);
constexpr unsigned cooldowns = layerc::editor::context_bit(EditorMenuContext::cooldowns);
constexpr unsigned bindings = layerc::editor::context_bit(EditorMenuContext::bindings);
constexpr unsigned panels = layerc::editor::context_bit(EditorMenuContext::panels);

// Captured from the editor before the table replaced the switches.
constexpr std::string_view shortcut_help_baseline =
    "Ctrl+N\tNew profile\n"
    "Ctrl+O\tImport...\n"
    "Ctrl+Shift+E\tExport JSON...\n"
    "Ctrl+Alt+Shift+E\tExport pack...\n"
    "Ctrl+S\tSave\n"
    "Ctrl+Shift+S\tSave As...\n"
    "Ctrl+Q\tQuit\n"
    "Ctrl+Z\tUndo\n"
    "Ctrl+Shift+Z\tRedo\n"
    "F2\tRename\n"
    "Ctrl+D\tDuplicate\n"
    "Ctrl+1\tProfile\n"
    "Ctrl+2\tCooldowns\n"
    "Ctrl+3\tBindings\n"
    "Ctrl+4\tPanels\n"
    "Ctrl+5\tAutomation\n"
    "Ctrl+6\tVisual\n"
    "Ctrl+,\tOptions...\n"
    "Ctrl+T\tDetected tasks...\n"
    "F1\tKeyboard shortcuts\n"
    "Ctrl+Y\tRedo\n"
    "Tab / Shift+Tab\tMove between fields\n"
    "Alt+letter\tOpen the menu with that underlined letter\n"
    "Right-click\tList context menu\n"
    "Arrow keys\tChange the focused list, choice or tab";

// The menu bar item paths in bar order, as the editor shipped them.
constexpr std::array<std::string_view, 40> menu_path_baseline{
    "&File/&New profile", "&File/&Import...", "&File/Export &JSON...",
    "&File/Export &pack...", "&File/&Save", "&File/Save &As...",
    "&File/Open profiles &folder", "&File/&Quit", "&Edit/&Undo", "&Edit/&Redo",
    "&Edit/Re&name", "&Profile/&Duplicate", "&Profile/&Delete...",
    "&Profile/Toggle saved &enabled", "&Cooldowns/&Add cooldown",
    "&Cooldowns/D&uplicate cooldown", "&Cooldowns/&Remove cooldown...",
    "&Cooldowns/Edit &groups...", "&Bindings/Add &key", "&Bindings/Add &mouse",
    "&Bindings/&Capture focused chord...", "&Bindings/&Remove binding",
    "&Panels/&Add panel", "&Panels/&Remove panel...", "&Panels/Choose &colours...",
    "&Panels/&Position on window", "&View/&Profile", "&View/&Cooldowns",
    "&View/&Bindings", "&View/&Panels", "&View/&Automation", "&View/&Visual",
    "&View/Daemon &status", "&View/Runtime &diagnostics...", "&Tools/&Options...",
    "&Tools/A&ppearance...", "&Tools/Split grouped &bars", "&Tools/Detected &tasks...",
    "&Help/&Keyboard shortcuts", "&Help/&About LayerE"};

// contexts and target contexts per command, in table order.
constexpr std::array<std::pair<unsigned, unsigned>, 40> context_baseline{{
    {profiles, 0},          {profiles, 0},          {profiles, 0},
    {profiles, 0},          {profiles, 0},          {0, 0},
    {0, 0},                 {0, 0},                 {0, 0},
    {0, 0},                 {0, 0},                 {profiles, profiles},
    {profiles, profiles},   {profiles, profiles},   {cooldowns, 0},
    {cooldowns, cooldowns}, {cooldowns, cooldowns}, {cooldowns, 0},
    {bindings, 0},          {bindings, 0},          {bindings, 0},
    {bindings, bindings},   {panels, 0},            {panels, panels},
    {panels, panels},       {0, 0},                 {0, 0},
    {0, 0},                 {0, 0},                 {0, 0},
    {0, 0},                 {0, 0},                 {0, 0},
    {0, 0},                 {0, 0},                 {0, 0},
    {0, 0},                 {0, 0},                 {0, 0},
    {0, 0},
}};

} // namespace

void linux_editor_command_registry_contracts() {
  setenv("FLTK_BACKEND", "x11", 1);
  const auto specs = layerc::editor::editor_command_specs();
  expect(specs.size() == static_cast<std::size_t>(EditorCommand::count),
         "the table holds exactly one row per command");
  bool ordered = true;
  for (std::size_t i = 0; i < specs.size(); ++i)
    ordered = ordered && specs[i].command == static_cast<EditorCommand>(i);
  expect(ordered, "the table is ordered by enumerator, so command_spec is an index");
  bool paths_match = true;
  std::size_t row{};
  for (const auto &spec : specs) {
    if (spec.menu_path == nullptr) continue;
    if (row >= menu_path_baseline.size() ||
        std::string_view(spec.menu_path) != menu_path_baseline[row])
      paths_match = false;
    ++row;
  }
  expect(paths_match && row == menu_path_baseline.size(),
         "every menu path keeps the shipped label and bar order");
  bool contexts_match = true;
  for (std::size_t i = 0; i < specs.size() && i < context_baseline.size(); ++i)
    contexts_match = contexts_match && specs[i].contexts == context_baseline[i].first &&
                     specs[i].target_contexts == context_baseline[i].second;
  expect(contexts_match,
         "context menus offer and target exactly the commands they did before");
  expect(layerc::editor::command_spec(EditorCommand::redo).alternate_shortcut ==
             (FL_CTRL | 'y'),
         "Ctrl+Y stays the alternate redo shortcut");
  expect(layerc::editor::shortcut_help() == shortcut_help_baseline,
         "the shortcut help is byte-identical to the shipped text");
}
