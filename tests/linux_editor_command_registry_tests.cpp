// The Linux editor command table: completeness, order, the menu/context truth
// tables and the derived help text.
#include "editor_commands.hpp"
#include "platform/hid_binding_capture.hpp"
#include "platform/services.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace {

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
    "Ctrl+Shift+S\tSave as...\n"
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
    "&File/Export &pack...", "&File/&Save", "&File/Save &as...",
    "&File/Open profiles &folder", "&File/&Quit", "&Edit/&Undo", "&Edit/&Redo",
    "&Edit/Re&name", "&Profile/&Duplicate", "&Profile/&Delete...",
    "&Profile/Toggle saved &enabled", "&Cooldowns/&Add cooldown",
    "&Cooldowns/D&uplicate cooldown", "&Cooldowns/&Remove cooldown...",
    "&Cooldowns/Edit &groups...", "&Bindings/Add &key", "&Bindings/Add &mouse",
    "&Bindings/&Capture focused chord...", "&Bindings/&Remove binding",
    "&Panels/&Add panel", "&Panels/&Remove panel...", "&Panels/Choose &colors...",
    "&Panels/&Position on window", "&View/&Profile", "&View/&Cooldowns",
    "&View/&Bindings", "&View/&Panels", "&View/&Automation", "&View/&Visual",
    "&View/LayerD &status", "&View/Runtime &diagnostics...", "&Tools/&Options...",
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

// Every shortcut row is "key<TAB>description" from the command table, F1
// included, so the dialog can measure two columns (DesignSystem.md, Keyboard).
void linux_editor_shortcuts_columns() {
  const std::string rows = layerc::editor::shortcut_help();
  expect(rows.find("F1\t") != std::string::npos, "F1 appears in the shortcut table");
  bool tabbed = true;
  std::size_t start = 0;
  while (start < rows.size()) {
    const auto end = rows.find('\n', start);
    const auto line =
        rows.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty())
      tabbed = tabbed && line.find('\t') != std::string::npos &&
               line.find('\t') == line.rfind('\t');
    if (end == std::string::npos) break;
    start = end + 1;
  }
  expect(tabbed, "every shortcut row has exactly one tab and no space-aligned columns");
  expect(rows.find("\tSave") != std::string::npos &&
             rows.find("\tUndo") != std::string::npos &&
             rows.find("\tRedo") != std::string::npos,
         "command-table shortcut rows appear with their keys");
}

void linux_editor_command_registry_contracts() {
#ifndef _WIN32
  setenv("FLTK_BACKEND", "x11", 1);
#endif
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
    if (spec.command == EditorCommand::capture_hid_binding && layerc::editor::hid_binding_capture_available()) {
      expect(spec.menu_path && std::string_view(spec.menu_path) == "&Bindings/&HID binding..." && row == 21,
             "Windows offers HID capture after focused chord capture");
      continue;
    }
    if (spec.menu_path == nullptr) continue;
    if (row >= menu_path_baseline.size() ||
        std::string_view(spec.menu_path) != menu_path_baseline[row])
      paths_match = false;
    ++row;
  }
  expect(paths_match && row == menu_path_baseline.size(),
         "every menu path uses the v2 label and keeps the bar order");
  bool contexts_match = true;
  row = 0;
  for (const auto &spec : specs) {
    if (spec.command == EditorCommand::capture_hid_binding) {
      if (layerc::editor::hid_binding_capture_available()) {
        expect(spec.menu_path && spec.contexts == bindings && spec.target_contexts == 0,
               "Windows offers HID capture in the binding context");
      } else {
        expect(spec.menu_path == nullptr && spec.contexts == 0 && spec.target_contexts == 0,
               "the Windows-only HID capture command is not offered on Linux");
      }
      continue;
    }
    if (row >= context_baseline.size() ||
        spec.contexts != context_baseline[row].first ||
        spec.target_contexts != context_baseline[row].second)
      contexts_match = false;
    ++row;
  }
  expect(contexts_match && row == context_baseline.size(),
         "context menus offer and target exactly the commands they did before");
  expect(layerc::editor::command_spec(EditorCommand::redo).alternate_shortcut ==
             (FL_CTRL | 'y'),
         "Ctrl+Y stays the alternate redo shortcut");
  if constexpr (layerc::editor::windows_editor_keys) {
    std::string expected(shortcut_help_baseline);
    expected.insert(expected.find("Tab / Shift+Tab"),
                    "Delete\tDelete the selected profile\n"
                    "Delete\tRemove the selected cooldown\n"
                    "Shift+F10\tOpen the focused list's context menu\n"
                    "Menu\tOpen the focused list's context menu\n");
    expect(layerc::editor::shortcut_help() == expected,
           "Windows shortcut help also includes its focused-list bindings");
  } else {
    expect(layerc::editor::shortcut_help() == shortcut_help_baseline,
           "the shortcut help uses the v2 labels and unchanged bindings");
  }
}
