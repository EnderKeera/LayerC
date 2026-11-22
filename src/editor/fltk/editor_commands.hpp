#pragma once

#include "editor_geometry.hpp"

#include <optional>
#include <span>
#include <string>

namespace layerc::editor {
class EditorApp;

// Menu order is the table order: the bar, the context menus, the shortcut
// dispatch and the help all read this one list.
enum class EditorCommand {
  new_profile, import_profiles, export_json, export_pack, save, save_as, open_folder, close,
  undo, redo, rename,
  duplicate, delete_profile, toggle_enabled,
  add_cooldown, duplicate_cooldown, delete_cooldown, cooldown_groups,
  add_keyboard, add_mouse, capture_binding, capture_hid_binding, delete_binding,
  add_panel, delete_panel, panel_colors, position,
  profile_page, cooldown_page, binding_page, panel_page, automation_page, visual_page,
  refresh, diagnostics,
  options, appearance, split_bars, tasks, shortcuts_help, about,
  count
};

enum class EditorMenuContext { profiles, cooldowns, bindings, panels };

inline constexpr unsigned context_bit(const EditorMenuContext context) noexcept {
  return 1U << static_cast<unsigned>(context);
}

struct EditorCommandSpec {
  EditorCommand command;
  const char *menu_path;     // nullptr when not in the menu bar
  const char *label;         // menu/help label with '&' mnemonics
  int shortcut;              // 0 = none
  int alternate_shortcut;    // e.g. Ctrl+Y for redo; 0 = none; listed in help
  int menu_flags;            // FL_MENU_DIVIDER etc.
  unsigned contexts;         // bitmask of EditorMenuContext rows offering it
  unsigned target_contexts;  // contexts where it needs a right-clicked row
  bool (*enabled)(const EditorApp &);
  void (*run)(EditorApp &);
};

struct EditorListShortcut {
  int shortcut;
  const char *keys;
  const char *label;
  unsigned contexts;
  std::optional<EditorCommand> command;
};

[[nodiscard]] std::span<const EditorListShortcut> editor_list_shortcuts();
[[nodiscard]] std::span<const EditorCommandSpec> editor_command_specs();
[[nodiscard]] const EditorCommandSpec &command_spec(EditorCommand command);
[[nodiscard]] bool command_enabled(const EditorApp &app, EditorCommand command);
// Runs the command when it is enabled; a disabled command is a no-op.
void run_command(EditorApp &app, EditorCommand command);
[[nodiscard]] std::string shortcut_help();

// Declares the table rows, whose lambdas need EditorApp's private state.
struct EditorCommandRegistry;

} // namespace layerc::editor
