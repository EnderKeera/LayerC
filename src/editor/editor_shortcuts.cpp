#include "editor_shortcuts.hpp"

#include "editor_resource.h"
#include "static_style.hpp"

#include <algorithm>

namespace layerc::editor {
namespace {

constexpr std::array shortcut_table{
    ShortcutEntry{L"Ctrl+N", L"New profile", {IDM_FILE_NEW, 0}},
    ShortcutEntry{L"Ctrl+S / Ctrl+Shift+S", L"Save / Save as",
                  {IDM_FILE_SAVE, IDM_FILE_SAVE_AS}},
    ShortcutEntry{L"Ctrl+Z / Ctrl+Y", L"Undo / Redo",
                  {IDM_EDIT_UNDO, IDM_EDIT_REDO}},
    ShortcutEntry{L"Ctrl+D", L"Duplicate the selected cooldown",
                  {IDM_COOLDOWN_DUPLICATE, 0}},
    ShortcutEntry{L"Ctrl+Shift+A", L"Show or hide advanced settings",
                  {IDM_VIEW_ADVANCED, 0}},
    ShortcutEntry{L"Ctrl+,", L"Options", {IDM_FILE_OPTIONS, 0}},
    ShortcutEntry{L"F2", L"Rename the selected profile or cooldown", {}},
    ShortcutEntry{L"Del", L"Delete the selected profile or cooldown", {}},
    ShortcutEntry{L"Shift+F10", L"Context menu", {}},
    ShortcutEntry{L"Up / Down, mouse wheel",
                  L"Step the focused number: Duration 0.1 s (Shift: 1 s), bar "
                  L"width and height 1 px",
                  {}},
    ShortcutEntry{L"Arrow keys",
                  L"Nudge the selected bar in the preview 1 px (Shift: 10 px)",
                  {}},
    ShortcutEntry{L"Alt+letter", L"Jump to the underlined field", {}},
    ShortcutEntry{L"F1", L"Keyboard shortcuts", {IDM_HELP_SHORTCUTS, 0}},
};

std::wstring_view virtual_key_name(const WORD key) {
  switch (key) {
  case VK_OEM_COMMA: return L",";
  case VK_OEM_PERIOD: return L".";
  case VK_OEM_PLUS: return L"+";
  case VK_OEM_MINUS: return L"-";
  case VK_DELETE: return L"Del";
  case VK_INSERT: return L"Ins";
  case VK_RETURN: return L"Enter";
  case VK_ESCAPE: return L"Esc";
  case VK_TAB: return L"Tab";
  case VK_SPACE: return L"Space";
  case VK_BACK: return L"Backspace";
  case VK_HOME: return L"Home";
  case VK_END: return L"End";
  case VK_PRIOR: return L"Page Up";
  case VK_NEXT: return L"Page Down";
  case VK_UP: return L"Up";
  case VK_DOWN: return L"Down";
  case VK_LEFT: return L"Left";
  case VK_RIGHT: return L"Right";
  default: return {};
  }
}

} // namespace

std::span<const ShortcutEntry> editor_shortcuts() noexcept {
  return shortcut_table;
}

std::wstring accelerator_text(const ACCEL &accelerator) {
  std::wstring text;
  if ((accelerator.fVirt & FCONTROL) != 0)
    text += L"Ctrl+";
  if ((accelerator.fVirt & FALT) != 0)
    text += L"Alt+";
  if ((accelerator.fVirt & FSHIFT) != 0)
    text += L"Shift+";
  const WORD key = accelerator.key;
  if ((accelerator.fVirt & FVIRTKEY) == 0 || (key >= L'A' && key <= L'Z') ||
      (key >= L'0' && key <= L'9'))
    text += static_cast<wchar_t>(key);
  else if (key >= VK_F1 && key <= VK_F24)
    text += L"F" + std::to_wstring(key - VK_F1 + 1);
  else if (const auto name = virtual_key_name(key); !name.empty())
    text += name;
  else
    text += L"Key " + std::to_wstring(key);
  return text;
}

std::wstring menu_action_text(const HMENU menu, const int command) {
  std::array<wchar_t, 128> buffer{};
  if (menu == nullptr ||
      GetMenuStringW(menu, static_cast<UINT>(command), buffer.data(),
                     static_cast<int>(buffer.size()), MF_BYCOMMAND) <= 0)
    return {};
  std::wstring text(buffer.data());
  if (const auto tab = text.find(L'\t'); tab != std::wstring::npos)
    text.resize(tab);
  std::erase(text, L'&');
  while (!text.empty() && (text.back() == L'.' || text.back() == L' '))
    text.pop_back();
  return text;
}

std::vector<ACCEL> editor_accelerators(const HINSTANCE instance) {
  const HACCEL table =
      LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDR_EDITOR_ACCELERATORS));
  if (table == nullptr)
    return {};
  std::vector<ACCEL> entries(
      static_cast<std::size_t>(std::max(0, CopyAcceleratorTableW(table, nullptr, 0))));
  if (!entries.empty())
    entries.resize(static_cast<std::size_t>(std::max(
        0, CopyAcceleratorTableW(table, entries.data(),
                                 static_cast<int>(entries.size())))));
  return entries;
}

std::vector<ShortcutRow>
shortcut_rows(const std::span<const ACCEL> accelerators,
              const std::function<std::wstring(int)> &action_of) {
  const auto table = editor_shortcuts();
  const auto described = [&](const int command) {
    return std::ranges::any_of(table, [command](const ShortcutEntry &entry) {
      return std::ranges::find(entry.commands, command) != entry.commands.end();
    });
  };
  std::vector<ShortcutRow> rows;
  for (const auto &entry : table.first(table.size() - 1))
    rows.push_back({std::wstring(entry.keys), std::wstring(entry.action)});
  for (const ACCEL &accelerator : accelerators) {
    const int command = accelerator.cmd;
    if (described(command))
      continue;
    if (auto action = action_of ? action_of(command) : std::wstring{};
        !action.empty())
      rows.push_back({accelerator_text(accelerator), std::move(action)});
  }
  rows.push_back({std::wstring(table.back().keys),
                  std::wstring(table.back().action)});
  return rows;
}

void create_shortcut_cells(const HWND dialog,
                           const std::span<const ShortcutRow> rows) {
  const auto instance =
      reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
  HWND after = HWND_TOP;
  for (std::size_t row = 0; row < rows.size(); ++row) {
    for (int column = 0; column < 2; ++column) {
      const auto &text = column == 0 ? rows[row].keys : rows[row].action;
      const int id = IDC_SHORTCUT_FIRST + static_cast<int>(row) * 2 + column;
      const HWND cell = CreateWindowExW(
          0, L"STATIC", text.c_str(),
          WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX, 0, 0, 0, 0,
          dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
          nullptr);
      if (cell == nullptr)
        continue;
      SetWindowPos(cell, after, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      after = cell;
      if (column == 1)
        set_text_tone(cell, TextTone::text);
    }
  }
}

} // namespace layerc::editor
