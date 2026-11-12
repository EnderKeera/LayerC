#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::editor {

// One row of Help > Keyboard shortcuts (DesignSystem.md, Keyboard), with the
// accelerator commands it describes; keys handled in code have none.
struct ShortcutEntry {
  std::wstring_view keys;
  std::wstring_view action;
  std::array<int, 2> commands{};
};

struct ShortcutRow {
  std::wstring keys;
  std::wstring action;
};

// The editor's command table; F1 is last.
[[nodiscard]] std::span<const ShortcutEntry> editor_shortcuts() noexcept;
// "Ctrl+Shift+S", "Ctrl+,", "F1": the keys as the menus write them.
[[nodiscard]] std::wstring accelerator_text(const ACCEL &accelerator);
// The menu item's words for a command, without its access-key marker,
// shortcut or trailing "...": "Save as". Empty when the menu lacks it.
[[nodiscard]] std::wstring menu_action_text(HMENU menu, int command);
[[nodiscard]] std::vector<ACCEL> editor_accelerators(HINSTANCE instance);
// The table, then any accelerator it does not describe, named by
// `action_of` (rows without a name are left out), with F1 kept last. A new
// shortcut so appears without editing the table.
[[nodiscard]] std::vector<ShortcutRow>
shortcut_rows(std::span<const ACCEL> accelerators,
              const std::function<std::wstring(int)> &action_of);
// Adds the rows' statics to the Keyboard shortcuts dialog from
// IDC_SHORTCUT_FIRST in reading order ahead of OK: each row's keys in
// `label`, then its action in `text`.
void create_shortcut_cells(HWND dialog, std::span<const ShortcutRow> rows);

} // namespace layerc::editor
