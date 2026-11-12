#include "accessible_text.hpp"
#include "dialog_layout.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_dialog_layouts.hpp"
#include "editor_resource.h"
#include "editor_shortcuts.hpp"
#include "editor_window.hpp"
#include "static_style.hpp"

#include "layerc/core/version.hpp"
#include "layerc/platform/settings_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windows.h>
#include <windowsx.h>

namespace layerc::editor {
namespace {

constexpr std::wstring_view placement_setting = L"EditorWindowPlacement";
constexpr UINT_PTR numeric_subclass_id = 0x4C45'0001;
constexpr wchar_t editor_title[] = L"LayerC Editor";

// Step and floor per numeric field; Shift multiplies the step by ten.
struct NumericField {
  int id;
  double step;
  double minimum;
  double maximum;
  int decimals;
};

constexpr std::array numeric_fields{
    NumericField{IDC_COOLDOWN_DURATION, 0.1, 0.1, 86400.0, 1},
    NumericField{IDC_BAR_WIDTH, 1.0, 1.0, 4096.0, 0},
    NumericField{IDC_BAR_HEIGHT, 1.0, 1.0, 4096.0, 0},
    NumericField{IDC_COOLDOWN_CHARGES, 1.0, 1.0, 99.0, 0},
};

const NumericField *numeric_field(const int id) {
  const auto found = std::ranges::find(numeric_fields, id, &NumericField::id);
  return found == numeric_fields.end() ? nullptr : &*found;
}

void step_numeric(const HWND edit, const NumericField &field, const int steps) {
  const auto text = window_text(edit);
  wchar_t *end = nullptr;
  double value = std::wcstod(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(value))
    value = field.minimum;
  const double scale = std::pow(10.0, field.decimals);
  value = std::round((value + field.step * steps) * scale) / scale;
  value = std::clamp(value, field.minimum, field.maximum);
  wchar_t formatted[32]{};
  swprintf_s(formatted, L"%.*f", field.decimals, value);
  // SetWindowText raises EN_CHANGE, which runs the field's normal binding.
  SetWindowTextW(edit, formatted);
  SendMessageW(edit, EM_SETSEL, 0, -1);
}

LRESULT CALLBACK numeric_edit_subclass(const HWND edit, const UINT message,
                                       const WPARAM wparam,
                                       const LPARAM lparam,
                                       const UINT_PTR id, DWORD_PTR) {
  const auto *field = numeric_field(GetDlgCtrlID(edit));
  const int multiplier = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 10 : 1;
  switch (message) {
  case WM_GETDLGCODE:
    if (lparam != 0) {
      const auto *msg = reinterpret_cast<const MSG *>(lparam);
      if (msg->message == WM_KEYDOWN &&
          (msg->wParam == VK_UP || msg->wParam == VK_DOWN))
        return DefSubclassProc(edit, message, wparam, lparam) | DLGC_WANTARROWS;
    }
    break;
  case WM_KEYDOWN:
    if (field != nullptr && (wparam == VK_UP || wparam == VK_DOWN)) {
      step_numeric(edit, *field, (wparam == VK_UP ? 1 : -1) * multiplier);
      return 0;
    }
    break;
  case WM_MOUSEWHEEL:
    if (field != nullptr && GetFocus() == edit) {
      const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
      if (notches != 0)
        step_numeric(edit, *field, notches * multiplier);
      return 0;
    }
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(edit, &numeric_edit_subclass, id);
    break;
  default:
    break;
  }
  return DefSubclassProc(edit, message, wparam, lparam);
}

void add_tool(const HWND tooltip, const HWND owner, const HWND control,
              const wchar_t *text) {
  if (tooltip == nullptr || control == nullptr)
    return;
  TTTOOLINFOW tool{};
  tool.cbSize = sizeof(tool);
  tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  tool.hwnd = owner;
  tool.uId = reinterpret_cast<UINT_PTR>(control);
  tool.lpszText = const_cast<LPWSTR>(text);
  SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

bool placement_visible(const WINDOWPLACEMENT &placement) {
  return MonitorFromRect(&placement.rcNormalPosition,
                         MONITOR_DEFAULTTONULL) != nullptr;
}

// The buttons that say why they are unavailable (DesignSystem.md, Button).
constexpr std::array reason_buttons{IDC_REMOVE_COOLDOWN, IDC_ITEM_ADD,
                                    IDC_ITEM_EDIT, IDC_ITEM_REMOVE};

} // namespace

bool set_button_reason(EditorShell &shell, const int button,
                       const std::wstring_view reason) {
  const auto found = std::ranges::find(
      shell.button_reasons, button, &std::pair<int, std::wstring>::first);
  if (found == shell.button_reasons.end()) {
    if (reason.empty())
      return false;
    shell.button_reasons.emplace_back(button, reason);
  } else if (found->second == reason) {
    return false;
  } else {
    found->second = reason;
  }
  set_accessible_description(GetDlgItem(shell.window, button), reason);
  return true;
}

std::wstring_view button_reason(const EditorShell &shell, const int button) {
  const auto found = std::ranges::find(
      shell.button_reasons, button, &std::pair<int, std::wstring>::first);
  return found == shell.button_reasons.end() ? std::wstring_view{}
                                             : std::wstring_view(found->second);
}

void place_button_reason_tools(const EditorShell &shell) {
  if (shell.tooltip == nullptr)
    return;
  for (const int id : reason_buttons) {
    TTTOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.hwnd = shell.window;
    tool.uId = static_cast<UINT_PTR>(id);
    if (const HWND button = GetDlgItem(shell.window, id);
        button != nullptr && GetWindowRect(button, &tool.rect) != FALSE)
      MapWindowPoints(nullptr, shell.window,
                      reinterpret_cast<POINT *>(&tool.rect), 2);
    SendMessageW(shell.tooltip, TTM_NEWTOOLRECTW, 0,
                 reinterpret_cast<LPARAM>(&tool));
  }
}

void EditorWindow::install_shell_helpers() {
  const HWND window = shell_.window;
  set_text(GetDlgItem(window, IDC_PROFILE_DELETE), L"\u2212");
  set_text(GetDlgItem(window, IDC_PROFILE_MORE), L"\u2026");
  // A button without a word takes its tooltip's words as its name
  // (DesignSystem.md, Screen readers).
  set_accessible_name(GetDlgItem(window, IDC_PROFILE_NEW), L"New profile");
  set_accessible_name(GetDlgItem(window, IDC_PROFILE_DELETE),
                      L"Delete profile");
  set_accessible_name(GetDlgItem(window, IDC_PROFILE_MORE),
                      L"More profile actions");
  describe_with(GetDlgItem(window, IDC_COOLDOWN_ACTIVE),
                GetDlgItem(window, IDC_COOLDOWN_ACTIVE_HINT));
  // Hints and reasons read in `subtext`; the contrast line and the status
  // strip's states lead with a drawn glyph.
  for (const int id : {IDC_POSITION_STATUS, IDC_COOLDOWN_ACTIVE_HINT,
                       IDC_VISUAL_HINT, IDC_COOLDOWN_REMOVE_REASON,
                       IDC_ITEM_REASON})
    set_text_tone(GetDlgItem(window, id), TextTone::subtext);
  set_static_style(GetDlgItem(window, IDC_OVERLAY_CONTRAST),
                   {.tone = TextTone::text,
                    .glyph = layerc::platform::StatusGlyph::warning});
  set_strip_status(GetDlgItem(window, IDC_CONNECTION_STATUS),
                   L"LayerD: connecting");
  set_strip_status(GetDlgItem(window, IDC_VISUAL_RUNTIME_STATUS),
                   shell_.visual_runtime_status);
  SendMessageW(shell_.application_path, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(
                   L"Type app.exe, Browse, or choose a running app"));
  for (const auto &field : numeric_fields) {
    if (const HWND edit = GetDlgItem(window, field.id); edit != nullptr)
      SetWindowSubclass(edit, &numeric_edit_subclass, numeric_subclass_id, 0);
  }
  shell_.tooltip = CreateWindowExW(
      WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
      WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, shell_.instance, nullptr);
  if (shell_.tooltip == nullptr)
    return;
  SetWindowTheme(shell_.tooltip, L"DarkMode_Explorer", nullptr);
  SendMessageW(shell_.tooltip, TTM_SETMAXTIPWIDTH, 0,
               dpi_scaled(360, window_base_dpi(window)));
  // A tooltip is also its control's description. One that changes is
  // written where its text does, and one that only repeats the name is not
  // said twice.
  const auto tip = [&](const int id, const wchar_t *text) {
    add_tool(shell_.tooltip, window, GetDlgItem(window, id), text);
  };
  const auto tool = [&](const int id, const wchar_t *text) {
    tip(id, text);
    set_accessible_description(GetDlgItem(window, id), text);
  };
  tool(IDC_PROFILE_NEW, L"New profile (Ctrl+N)");
  tip(IDC_PROFILE_DELETE, L"Delete profile");
  tip(IDC_PROFILE_MORE, L"More profile actions");
  tool(IDC_ADVANCED,
       L"Show profile ID, exact path matching, HID bindings and cooldown "
       L"policies (Ctrl+Shift+A)");
  tip(IDC_APPLICATION_PATH, LPSTR_TEXTCALLBACKW);
  tool(IDC_BROWSE_APPLICATION, L"Choose the game's .exe file");
  tool(IDC_CHOOSE_APPLICATION, L"Pick from applications running now");
  tool(IDC_COOLDOWN_DURATION, L"Up / Down or the mouse wheel step 0.1 s "
                              L"(Shift: 1 s)");
  tool(IDC_CAPTURE_BINDING, L"Press the key or chord that starts this "
                            L"cooldown");
  tool(IDC_CAPTURE_HID, L"Bind a control on a generic HID device");
  tool(IDC_POSITION_ON_WINDOW,
       L"Place the bars directly on the running game window");
  tip(IDC_PREVIEW, preview_tooltip);
  tool(IDC_BAR_WIDTH, L"Up / Down or the mouse wheel step 1 px");
  tool(IDC_BAR_HEIGHT, L"Up / Down or the mouse wheel step 1 px");
  // The binding shows key names; its tooltip carries the scan codes.
  tip(IDC_COOLDOWN_BINDING, LPSTR_TEXTCALLBACKW);
  // A disabled button takes no mouse input, so its reason is a tool of the
  // window over the button's rectangle.
  for (const int id : reason_buttons) {
    TTTOOLINFOW reason{};
    reason.cbSize = sizeof(reason);
    reason.uFlags = TTF_SUBCLASS;
    reason.hwnd = window;
    reason.uId = static_cast<UINT_PTR>(id);
    reason.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(shell_.tooltip, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&reason));
  }
  place_button_reason_tools(shell_);
}

std::optional<LRESULT> EditorWindow::handle_shell_notify(const NMHDR &header) {
  if (header.code != TTN_GETDISPINFOW || shell_.tooltip == nullptr)
    return std::nullopt;
  auto &info = const_cast<NMTTDISPINFOW &>(
      reinterpret_cast<const NMTTDISPINFOW &>(header));
  const HWND control = reinterpret_cast<HWND>(header.idFrom);
  shell_.tooltip_text.clear();
  if ((info.uFlags & TTF_IDISHWND) == 0) {
    // A reason tool: its id is the disabled button's.
    shell_.tooltip_text =
        button_reason(shell_, static_cast<int>(header.idFrom));
  } else if (control == shell_.application_path) {
    shell_.tooltip_text = application_match_text();
  } else if (control == GetDlgItem(shell_.window, IDC_COOLDOWN_BINDING)) {
    shell_.tooltip_text = cooldown_editor_.binding_codes_text();
  }
  info.lpszText = shell_.tooltip_text.data();
  return 0;
}

void EditorWindow::browse_application() {
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return;
  constexpr COMDLG_FILTERSPEC filters[] = {{L"Applications", L"*.exe"}};
  dialog->SetFileTypes(1, filters);
  dialog->SetTitle(L"Choose the game application");
  FILEOPENDIALOGOPTIONS options{};
  if (SUCCEEDED(dialog->GetOptions(&options)))
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM |
                       FOS_NOCHANGEDIR);
  std::wstring chosen;
  if (SUCCEEDED(dialog->Show(shell_.window))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        chosen = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  if (chosen.empty())
    return;
  const bool exact_path = owner_draw_checkbox_checked(
      GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH));
  (void)apply_profile_change(
      ChangeKind::immediate, [&](layerc::core::GameProfile &profile) {
        auto &game = profile.game;
        game.executable_name =
            wide_to_utf8(std::filesystem::path(chosen).filename().wstring());
        if (exact_path)
          game.executable_path = wide_to_utf8(chosen);
        else
          game.executable_path.clear();
      });
  {
    auto sync = shell_.sync_scope();
    const auto &game = document_.profile().game;
    set_text(shell_.application_path,
             to_wide(game.executable_path.empty() ? game.executable_name
                                                  : game.executable_path));
  }
  describe_application();
  set_status(L"Application set to " + to_wide(document_.profile().game.executable_name) + L".");
}

std::wstring EditorWindow::application_match_text() const {
  const auto &game = document_.profile().game;
  return !game.executable_path.empty()
             ? L"Matches exactly " + to_wide(game.executable_path)
         : !game.executable_name.empty()
             ? L"Matches any " + to_wide(game.executable_name) +
                   L" regardless of folder"
             : L"No application chosen yet";
}

void EditorWindow::describe_application() {
  set_accessible_description(shell_.application_path,
                             application_match_text());
}

// Typed text is an executable name ("game.exe") or a full path; a path only
// pins the exact location when "Match exact path" is on.
void EditorWindow::apply_application_text() {
  if (shell_.syncing)
    return;
  std::wstring text = window_text(shell_.application_path);
  const auto first = text.find_first_not_of(L" \t\"");
  const auto last = text.find_last_not_of(L" \t\"");
  text = first == std::wstring::npos ? std::wstring{}
                                     : text.substr(first, last - first + 1);
  const bool path_like = text.find_first_of(L"\\/") != std::wstring::npos;
  const std::filesystem::path typed(text);
  const bool exact = owner_draw_checkbox_checked(
      GetDlgItem(shell_.window, IDC_APPLICATION_EXACT_PATH));
  (void)apply_profile_change(ChangeKind::text, [&](layerc::core::GameProfile &candidate) {
    auto &game = candidate.game;
    game.executable_name = wide_to_utf8(path_like ? typed.filename().wstring() : text);
    game.executable_path =
        exact && path_like && typed.is_absolute() ? wide_to_utf8(text) : std::string{};
  });
  describe_application();
}

void EditorWindow::refresh_title() {
  if (shell_.window == nullptr)
    return;
  const bool draft = document_.path().empty();
  const bool modified = has_unsaved_changes();
  std::wstring name = to_wide(document_.profile().name);
  if (name.empty())
    name = L"Untitled profile";
  const std::wstring title = (modified ? L"* " : L"") + name +
                             (draft ? L" (unsaved)" : L"") + L" - " +
                             editor_title;
  if (title != shell_.title_cache) {
    shell_.title_cache = title;
    SetWindowTextW(shell_.window, title.c_str());
    set_text(GetDlgItem(shell_.window, IDC_PROFILE_TITLE),
             (modified ? L"\u2022 " : L"") + name);
  }
}

void EditorWindow::sync_draft_row() {
  auto sync = shell_.sync_scope();
  const bool has_draft_row =
      !shell_.profiles.empty() && shell_.profiles.front().path.empty();
  const bool draft = document_.path().empty();
  if (has_draft_row && !draft) {
    shell_.profiles.erase(shell_.profiles.begin());
    ListView_DeleteItem(shell_.profile_list, 0);
  } else if (!has_draft_row && draft) {
    ProfileEntry entry;
    entry.name = L"New profile";
    entry.executable = L"unsaved";
    entry.label = L"New profile (unsaved)";
    entry.enabled = true;
    shell_.profiles.insert(shell_.profiles.begin(), std::move(entry));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = 0;
    item.pszText = shell_.profiles.front().label.data();
    ListView_InsertItem(shell_.profile_list, &item);
  }
  if (draft) {
    ListView_SetItemState(shell_.profile_list, -1, 0,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(shell_.profile_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(shell_.profile_list, 0, FALSE);
  }
  refresh_title();
}

void EditorWindow::focus_field(const int id) {
  const HWND field = GetDlgItem(shell_.window, id);
  if (field == nullptr || IsWindowEnabled(field) == FALSE)
    return;
  SendMessageW(shell_.window, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(field),
               TRUE);
  SendMessageW(field, EM_SETSEL, 0, -1);
}

void EditorWindow::show_context_menu(const HWND source, POINT screen) {
  const bool profiles = source == shell_.profile_list;
  if (!profiles && source != cooldown_editor_.list())
    return;
  const bool keyboard = screen.x == -1 && screen.y == -1;
  if (profiles) {
    int row = -1;
    if (keyboard) {
      row = ListView_GetNextItem(shell_.profile_list, -1, LVNI_SELECTED);
    } else {
      LVHITTESTINFO hit{};
      hit.pt = screen;
      ScreenToClient(shell_.profile_list, &hit.pt);
      row = ListView_HitTest(shell_.profile_list, &hit);
      // Right-click selects (and opens) the row first, like Explorer.
      if (row >= 0)
        ListView_SetItemState(shell_.profile_list, row,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
      row = ListView_GetNextItem(shell_.profile_list, -1, LVNI_SELECTED);
    }
    if (row < 0)
      return;
    if (keyboard) {
      RECT bounds{};
      ListView_GetItemRect(shell_.profile_list, row, &bounds, LVIR_BOUNDS);
      screen = {bounds.left + 24, bounds.bottom};
      ClientToScreen(shell_.profile_list, &screen);
    }
  } else {
    if (!keyboard) {
      POINT client = screen;
      ScreenToClient(cooldown_editor_.list(), &client);
      const LRESULT hit = SendMessageW(cooldown_editor_.list(), LB_ITEMFROMPOINT,
                                       0, MAKELPARAM(client.x, client.y));
      if (HIWORD(hit) == 0 &&
          SendMessageW(cooldown_editor_.list(), LB_GETCURSEL, 0, 0) !=
              LOWORD(hit)) {
        SendMessageW(cooldown_editor_.list(), LB_SETCURSEL, LOWORD(hit), 0);
        SendMessageW(shell_.window, WM_COMMAND,
                     MAKEWPARAM(IDC_COOLDOWN_LIST, LBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(cooldown_editor_.list()));
      }
    } else {
      RECT bounds{};
      const LRESULT row =
          SendMessageW(cooldown_editor_.list(), LB_GETCURSEL, 0, 0);
      SendMessageW(cooldown_editor_.list(), LB_GETITEMRECT,
                   static_cast<WPARAM>(std::max<LRESULT>(0, row)),
                   reinterpret_cast<LPARAM>(&bounds));
      screen = {bounds.left + 24, bounds.bottom};
      ClientToScreen(cooldown_editor_.list(), &screen);
    }
    if (cooldown_editor_.selected() == nullptr)
      return;
  }
  const HMENU menus =
      LoadMenuW(shell_.instance, MAKEINTRESOURCEW(IDR_CONTEXT_MENUS));
  if (menus == nullptr)
    return;
  const HMENU menu = GetSubMenu(menus, profiles ? 0 : 1);
  const bool draft = document_.path().empty();
  if (profiles) {
    ModifyMenuW(menu, IDM_PROFILE_TOGGLE, MF_BYCOMMAND | MF_STRING,
                IDM_PROFILE_TOGGLE,
                document_.profile().enabled ? L"&Turn off" : L"&Turn on");
    EnableMenuItem(menu, IDM_PROFILE_TOGGLE,
                   MF_BYCOMMAND | (draft ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, IDM_FILE_EXPORT_PACK,
                   MF_BYCOMMAND | (draft ? MF_GRAYED : MF_ENABLED));
  } else {
    EnableMenuItem(menu, IDM_COOLDOWN_REMOVE,
                   MF_BYCOMMAND | (document_.profile().cooldowns.size() > 1
                                       ? MF_ENABLED
                                       : MF_GRAYED));
  }
  const UINT command = static_cast<UINT>(TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, screen.x,
      screen.y, shell_.window, nullptr));
  DestroyMenu(menus);
  if (command != 0)
    SendMessageW(shell_.window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

bool EditorWindow::handle_list_key(const MSG &message) {
  if (message.message != WM_KEYDOWN)
    return false;
  const bool plain = (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                     (GetKeyState(VK_MENU) & 0x8000) == 0;
  if (!plain)
    return false;
  if (message.hwnd == GetDlgItem(shell_.window, IDC_PANEL_ITEMS)) {
    if (message.wParam == VK_DELETE)
      return handle_item_command(IDC_ITEM_REMOVE, BN_CLICKED);
    if (message.wParam == VK_F2) {
      open_panel_item_dialog(true);
      return true;
    }
  }
  if (message.hwnd == shell_.profile_list) {
    if (message.wParam == VK_DELETE) {
      delete_profile();
      return true;
    }
    if (message.wParam == VK_F2) {
      focus_field(IDC_PROFILE_NAME);
      return true;
    }
    if (message.wParam == VK_APPS ||
        (message.wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
      show_context_menu(shell_.profile_list, {-1, -1});
      return true;
    }
  } else if (message.hwnd == cooldown_editor_.list()) {
    if (message.wParam == VK_DELETE) {
      cooldown_editor_.remove();
      return true;
    }
    if (message.wParam == VK_F2) {
      focus_field(IDC_COOLDOWN_NAME);
      return true;
    }
  }
  return false;
}

void EditorWindow::duplicate_profile() {
  (void)apply_profile_controls(DocumentChangeKind::text);
  if (!confirm_unsaved_changes())
    return;
  auto copy = document_.profile();
  copy.name += " copy";
  copy.id = layerc::core::slugify(copy.name, "profile");
  end_edit_on_window();
  // Visual triggers and automation reference this profile's own assets and
  // rules; the copy starts without them. A copy draft is dirty by construction.
  document_.start_draft(std::move(copy), DraftOrigin::copy);
  refresh_profile_controls();
  visual_editor_.refresh();
  sync_draft_row();
  set_status(L"Profile duplicated without its visual trigger and automation "
             L"— Save to create it.");
}

void EditorWindow::set_advanced_visible(const bool visible, const bool persist) {
  shell_.advanced_visible = visible;
  set_text(GetDlgItem(shell_.window, IDC_ADVANCED),
           visible ? L"Hide advanced" : L"Show advanced");
  if (persist)
    (void)layerc::platform::write_bool_setting(
        layerc::platform::BoolSetting::editor_advanced_view, visible);
  layout();
}

void EditorWindow::set_text_scale(const int percent) {
  if (!layerc::platform::write_layerc_registry_dword(
          text_scale_setting, static_cast<std::uint32_t>(percent)))
    set_status(L"Could not save the text size.");
  layerc::platform::set_native_text_scale(percent);
  reload_fonts();
  set_status(L"Text size " + std::to_wstring(percent) +
             L" %. Dialogs use it the next time they open.");
}

void EditorWindow::update_view_menu(const HMENU popup) const {
  CheckMenuItem(popup, IDM_VIEW_ADVANCED,
                MF_BYCOMMAND |
                    (shell_.advanced_visible ? MF_CHECKED : MF_UNCHECKED));
  const int scale = layerc::platform::native_text_scale();
  CheckMenuRadioItem(popup, IDM_VIEW_TEXT_100, IDM_VIEW_TEXT_130,
                     scale >= 130   ? IDM_VIEW_TEXT_130
                     : scale >= 115 ? IDM_VIEW_TEXT_115
                                    : IDM_VIEW_TEXT_100,
                     MF_BYCOMMAND);
}

void EditorWindow::save_window_placement() const {
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (shell_.window == nullptr ||
      GetWindowPlacement(shell_.window, &placement) == FALSE)
    return;
  const auto *bytes = reinterpret_cast<const std::byte *>(&placement);
  (void)layerc::platform::write_layerc_registry_binary(
      placement_setting, std::span(bytes, sizeof(placement)));
}

bool EditorWindow::restore_window_placement(const int show_command) {
  const auto stored = layerc::platform::read_layerc_registry_binary(
      placement_setting, sizeof(WINDOWPLACEMENT));
  if (!stored.value || stored.value->size() != sizeof(WINDOWPLACEMENT))
    return false;
  WINDOWPLACEMENT placement{};
  std::memcpy(&placement, stored.value->data(), sizeof(placement));
  if (placement.length != sizeof(placement) || !placement_visible(placement))
    return false;
  placement.flags = 0;
  placement.showCmd =
      placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED
      : show_command == SW_SHOWMINIMIZED || show_command == SW_MINIMIZE ||
              show_command == SW_SHOWMINNOACTIVE
          ? static_cast<UINT>(show_command)
          : SW_SHOWNORMAL;
  return SetWindowPlacement(shell_.window, &placement) != FALSE;
}

void EditorWindow::refresh_connection_status() {
  const wchar_t *text =
      !live_edit_.connected()      ? L"LayerD: not running"
      : live_edit_.authenticated()   ? L"LayerD: connected"
                                             : L"LayerD: connecting";
  if (shell_.connection_cache != text) {
    shell_.connection_cache = text;
    set_strip_status(GetDlgItem(shell_.window, IDC_CONNECTION_STATUS), text);
  }
}

void EditorWindow::paint_status_strip(const HDC dc) const {
  if (shell_.status_strip_top <= 0)
    return;
  RECT client{};
  GetClientRect(shell_.window, &client);
  const auto strip_colors = colors(layerc::platform::EditorControlRole::status);
  RECT strip{client.left, shell_.status_strip_top, client.right, client.bottom};
  fill_solid_rect(dc, strip, strip_colors.fill);
  RECT divider{client.left, shell_.status_strip_top, client.right,
               shell_.status_strip_top + 1};
  fill_solid_rect(dc, divider, strip_colors.divider);
}

void EditorWindow::show_about() {
  OSVERSIONINFOEXW os{};
  os.dwOSVersionInfoSize = sizeof(os);
  using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOEXW *);
  if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
    const FARPROC procedure = GetProcAddress(ntdll, "RtlGetVersion");
    RtlGetVersion get_version{};
    static_assert(sizeof(get_version) == sizeof(procedure));
    std::memcpy(&get_version, &procedure, sizeof(get_version));
    if (get_version != nullptr)
      (void)get_version(&os);
  }
#ifdef NDEBUG
  constexpr std::wstring_view build = L"release";
#else
  constexpr std::wstring_view build = L"debug";
#endif
  const std::wstring version =
      to_wide(std::string(layerc::core::product_version));
  const std::wstring details =
      L"LayerC Editor " + version + L" (" + std::wstring(build) +
      L" build)\nWindows " + std::to_wstring(os.dwMajorVersion) + L"." +
      std::to_wstring(os.dwMinorVersion) + L" build " +
      std::to_wstring(os.dwBuildNumber) + L"\nText size " +
      std::to_wstring(layerc::platform::native_text_percent(
          GetDpiForWindow(shell_.window))) +
      L" %, " + std::to_wstring(GetDpiForWindow(shell_.window)) + L" DPI\n" +
      shell_.connection_cache + L"\n" + shell_.visual_runtime_status;
  const std::wstring text =
      L"LayerC Editor " + version +
      L"\n\nCreates and edits cooldown overlay profiles. LayerC reads input "
      L"passively: it never blocks, changes or sends input.\n\n"
      L"Source and releases: github.com/EnderKeera/LayerC";
  static constexpr PromptButton buttons[] = {{L"Copy details", 1},
                                             {L"Close", IDCANCEL}};
  if (themed_prompt(shell_.window, text, L"About LayerC Editor", buttons, 1) !=
      1)
    return;
  if (OpenClipboard(shell_.window) == FALSE)
    return;
  EmptyClipboard();
  const std::size_t bytes = (details.size() + 1) * sizeof(wchar_t);
  if (const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
      memory != nullptr) {
    if (void *target = GlobalLock(memory); target != nullptr) {
      std::memcpy(target, details.c_str(), bytes);
      GlobalUnlock(memory);
      if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
        GlobalFree(memory);
      else
        set_status(L"Version and diagnostics details copied.");
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

void EditorWindow::show_keyboard_shortcuts() {
  DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_SHORTCUTS), shell_.window,
      &dispatch_member_dialog<EditorWindow,
                              &EditorWindow::initialize_shortcuts,
                              &EditorWindow::handle_shortcuts_message>,
      reinterpret_cast<LPARAM>(this));
}

// One row per shortcut (DesignSystem.md, Keyboard): the keys in a single
// left-aligned column in `label`, the action beside them in `text`.
void EditorWindow::initialize_shortcuts(const HWND window) {
  apply_dialog_chrome(window, shell_.capture_exclusion);
  const HMENU menu = GetMenu(shell_.window);
  const auto rows = shortcut_rows(
      editor_accelerators(shell_.instance),
      [menu](const int command) { return menu_action_text(menu, command); });
  create_shortcut_cells(window, rows);
  style_dialog_children(window);
  layout_shortcuts_dialog(window, rows.size());
  center_over_owner(window);
}

LRESULT EditorWindow::handle_shortcuts_message(const HWND window,
                                               const UINT message,
                                               const WPARAM wparam,
                                               const LPARAM lparam) {
  if (message == WM_COMMAND &&
      (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
    EndDialog(window, IDOK);
    return TRUE;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
