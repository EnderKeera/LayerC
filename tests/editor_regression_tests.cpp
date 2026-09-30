#include "dialog_layout.hpp"
#include "editor_resource.h"
#include "editor_window.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <commctrl.h>
#include <windows.h>

namespace layerc::editor {

// Exercise the real leave decisions and renderers without running the editor,
// connecting to the daemon, or reading/writing the user's settings or profiles.
struct EditorWindowTestAccess {
  static ProfileDocument &document(EditorWindow &editor) {
    return editor.document_;
  }
  static void new_profile(EditorWindow &editor, const bool confirm = true) {
    editor.new_profile(confirm);
  }
  static void edit(EditorWindow &editor) {
    auto &document = editor.document_;
    document.profile().name += " edited";
    document.visual().emplace();
    document.visual()->profile_id = document.profile().id;
    document.automation().emplace();
    document.automation()->profile_id = document.profile().id;
    editor.record_profile_change(EditorWindow::ChangeKind::immediate);
  }
  static void duplicate(EditorWindow &editor) {
    // There are no edit controls in this fixture; keep the prepared document.
    auto sync = editor.shell_.sync_scope();
    editor.duplicate_profile();
  }
  static void undo(EditorWindow &editor) { editor.undo(); }
  static void redo(EditorWindow &editor) { editor.redo(); }
  static void close(EditorWindow &editor, const HWND window) {
    (void)editor.handle_main_message(window, WM_CLOSE, 0, 0);
  }
  static void open(EditorWindow &editor, const std::filesystem::path &path) {
    ProfileEntry entry;
    entry.path = path;
    editor.shell_.profiles.push_back(std::move(entry));
    editor.open_profile_row(static_cast<int>(editor.shell_.profiles.size() - 1),
                            true);
  }
  static void set_owner_font(EditorWindow &editor, const HFONT font) {
    editor.shell_.font = font;
  }
  static HFONT owner_font(const EditorWindow &editor) {
    return editor.shell_.font;
  }
  static void draw(EditorWindow &editor, const HWND dialog,
                   const DRAWITEMSTRUCT &item) {
    (void)editor.dialog_theme_message(
        dialog, WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
  }
  static void custom_draw(EditorWindow &editor, const HWND dialog,
                          const NMCUSTOMDRAW &item) {
    (void)editor.dialog_theme_message(
        dialog, WM_NOTIFY, item.hdr.idFrom, reinterpret_cast<LPARAM>(&item));
  }
};

} // namespace layerc::editor

namespace {

using layerc::editor::EditorWindowTestAccess;
using layerc::test::expect;

INT_PTR CALLBACK fixture_dialog_proc(HWND, UINT, WPARAM, LPARAM) {
  return FALSE;
}

class PromptAnswer final {
public:
  explicit PromptAnswer(const bool cancel) : cancel_(cancel) {
    active_ = this;
    timer_ = SetTimer(nullptr, 0, 10, &on_timer);
    expect(timer_ != 0, "the disposable prompt responder should start");
  }
  ~PromptAnswer() {
    if (timer_ != 0)
      KillTimer(nullptr, timer_);
    active_ = nullptr;
  }
  [[nodiscard]] int count() const noexcept { return count_; }

private:
  static void CALLBACK on_timer(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](const HWND window, LPARAM) -> BOOL {
          if (active_ == nullptr ||
              GetDlgItem(window, IDC_PROMPT_TEXT) == nullptr)
            return TRUE;
          ++active_->count_;
          // Messages go only to this test's own modal prompt, not input/game
          // windows. No input synthesis, hooks, or production data are used.
          const int id = active_->cancel_ ? IDC_PROMPT_BUTTON3
                                         : IDC_PROMPT_BUTTON2;
          SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
          return FALSE;
        },
        0);
  }

  static inline PromptAnswer *active_{};
  UINT_PTR timer_{};
  bool cancel_{};
  int count_{};
};

layerc::editor::EditorProfileSnapshot
snapshot(const layerc::editor::ProfileDocument &document) {
  return {document.profile(), document.visual(), document.automation()};
}

enum class DraftState { untouched, edited, duplicate, undone, redone };
enum class LeaveAction { close, switch_profile, new_profile };

void prepare(layerc::editor::EditorWindow &editor, const DraftState state) {
  EditorWindowTestAccess::new_profile(editor, false);
  if (state == DraftState::duplicate) {
    EditorWindowTestAccess::duplicate(editor);
  } else if (state != DraftState::untouched) {
    EditorWindowTestAccess::edit(editor);
    if (state == DraftState::undone || state == DraftState::redone)
      EditorWindowTestAccess::undo(editor);
    if (state == DraftState::redone)
      EditorWindowTestAccess::redo(editor);
  }
}

void leave(layerc::editor::EditorWindow &editor, const LeaveAction action,
           const HWND window, const std::filesystem::path &target) {
  switch (action) {
  case LeaveAction::close:
    EditorWindowTestAccess::close(editor, window);
    break;
  case LeaveAction::switch_profile:
    EditorWindowTestAccess::open(editor, target);
    break;
  case LeaveAction::new_profile:
    EditorWindowTestAccess::new_profile(editor);
    break;
  }
}

class Canvas final {
public:
  static constexpr int width = 512;
  static constexpr int height = 80;

  Canvas() {
    dc_ = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
    if (dc_ != nullptr && bitmap_ != nullptr)
      previous_ = SelectObject(dc_, bitmap_);
  }
  ~Canvas() {
    if (previous_ != nullptr)
      SelectObject(dc_, previous_);
    if (bitmap_ != nullptr)
      DeleteObject(bitmap_);
    if (dc_ != nullptr)
      DeleteDC(dc_);
  }
  [[nodiscard]] bool valid() const noexcept {
    return dc_ != nullptr && bitmap_ != nullptr && pixels_ != nullptr;
  }
  [[nodiscard]] HDC dc() const noexcept { return dc_; }
  void clear() {
    GdiFlush();
    std::fill_n(static_cast<std::uint32_t *>(pixels_), width * height, 0U);
  }
  [[nodiscard]] std::vector<std::uint32_t> pixels() const {
    GdiFlush();
    const auto *begin = static_cast<const std::uint32_t *>(pixels_);
    return {begin, begin + width * height};
  }

private:
  HDC dc_{};
  HBITMAP bitmap_{};
  HGDIOBJ previous_{};
  void *pixels_{};
};

std::vector<std::uint32_t>
render(layerc::editor::EditorWindow &editor, const HWND dialog,
       const HWND control, const UINT type, Canvas &canvas) {
  canvas.clear();
  if (type == ODT_BUTTON &&
      (GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW) {
    NMCUSTOMDRAW draw{};
    draw.hdr.hwndFrom = control;
    draw.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(control));
    draw.hdr.code = NM_CUSTOMDRAW;
    draw.dwDrawStage = CDDS_PREPAINT;
    draw.hdc = canvas.dc();
    GetClientRect(control, &draw.rc);
    EditorWindowTestAccess::custom_draw(editor, dialog, draw);
    expect(GetWindowLongPtrW(dialog, DWLP_MSGRESULT) == CDRF_SKIPDEFAULT,
           "checkbox/radio drawing should use the custom renderer");
  } else {
    DRAWITEMSTRUCT draw{};
    draw.CtlType = type;
    draw.CtlID = static_cast<UINT>(GetDlgCtrlID(control));
    draw.itemID = 0;
    draw.hwndItem = control;
    draw.hDC = canvas.dc();
    GetClientRect(control, &draw.rcItem);
    EditorWindowTestAccess::draw(editor, dialog, draw);
  }
  return canvas.pixels();
}

} // namespace

void editor_draft_leave_contracts() {
  const layerc::test::TempDirectory fixtures("layerc-draft-leave");
  const auto target_path = fixtures.path() / "target.json";
  auto target = layerc::editor::make_new_profile();
  target.id = "stored-target";
  target.name = "Stored target";
  target.game.executable_name = "fixture.exe";
  const auto target_json = layerc::core::serialize_profile_json(target);
  auto parsed_target = layerc::core::parse_profile_json(target_json);
  if (!expect(std::holds_alternative<layerc::core::GameProfile>(parsed_target),
              "the disposable switch target should round-trip through core JSON"))
    return;
  target = std::get<layerc::core::GameProfile>(std::move(parsed_target));
  if (!expect(layerc::test::write_file(target_path, target_json),
              "the switch target should be a disposable valid profile"))
    return;

  constexpr std::array states{DraftState::untouched, DraftState::edited,
                              DraftState::duplicate, DraftState::undone,
                              DraftState::redone};
  constexpr std::array actions{LeaveAction::close, LeaveAction::switch_profile,
                               LeaveAction::new_profile};
  for (const auto state : states) {
    for (const auto action : actions) {
      for (const bool cancel : {true, false}) {
        const auto editor_fixture =
            std::make_unique<layerc::editor::EditorWindow>();
        auto &editor = *editor_fixture;
        prepare(editor, state);
        auto &document = EditorWindowTestAccess::document(editor);
        const auto before = snapshot(document);
        const bool dirty = state != DraftState::untouched;
        expect(document.path().empty() && document.dirty() == dirty,
               "only untouched New Profile should begin clean");
        const HWND window = CreateWindowExW(
            0, L"STATIC", L"Draft leave fixture", WS_OVERLAPPED, 0, 0, 100,
            100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!expect(window != nullptr, "the disposable close target should load"))
          return;
        PromptAnswer answer(cancel);
        leave(editor, action, window, target_path);
        expect(answer.count() == (dirty ? 1 : 0),
               "close/switch/new must prompt for edited, duplicate, undo and "
               "redo drafts, but not untouched New Profile");
        if (dirty && cancel) {
          expect(IsWindow(window) != FALSE && snapshot(document) == before &&
                     document.path().empty() && document.dirty(),
                 "Cancel must abort close/switch/new and retain the entire draft");
        } else if (action == LeaveAction::close) {
          expect(IsWindow(window) == FALSE, "approved close should destroy its target");
        } else if (action == LeaveAction::switch_profile) {
          expect(document.path() == target_path && document.profile() == target &&
                     !document.dirty(),
                 "approved switch should load only the disposable target");
        } else {
          expect(document.path().empty() && !document.dirty(),
                 "approved New Profile should replace the draft with a clean one");
        }
        if (window != nullptr && IsWindow(window) != FALSE)
          DestroyWindow(window);
      }
    }
  }
}

void editor_dialog_font_contracts() {
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_OPTIONS),
                                         nullptr, &fixture_dialog_proc, 0);
  expect(dialog != nullptr, "the disposable font dialog should load");
  if (dialog == nullptr)
    return;
  const HFONT small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
  const HFONT large = CreateFontW(-36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
  Canvas canvas;
  if (expect(small != nullptr && large != nullptr && canvas.valid(),
             "distinct fonts and a disposable bitmap should be available")) {
    const auto editor_fixture =
        std::make_unique<layerc::editor::EditorWindow>();
    auto &editor = *editor_fixture;
    layerc::editor::DialogChrome chrome;
    expect(chrome.attach(dialog, 96), "the fixture should own its dialog font");
    const std::array classes{L"BUTTON", L"BUTTON", L"BUTTON", L"LISTBOX", L"COMBOBOX"};
    constexpr std::array<DWORD, 5> styles{
        BS_OWNERDRAW, BS_AUTOCHECKBOX, BS_AUTORADIOBUTTON,
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS};
    constexpr std::array<UINT, 5> types{ODT_BUTTON, ODT_BUTTON, ODT_BUTTON,
                                       ODT_LISTBOX, ODT_COMBOBOX};
    for (std::size_t index = 0; index < types.size(); ++index) {
      const HWND control = CreateWindowExW(
          0, classes[index], L"Control-local font", WS_CHILD | styles[index],
          0, 0, Canvas::width, Canvas::height, dialog,
          reinterpret_cast<HMENU>(static_cast<INT_PTR>(20000 + index)),
          instance, nullptr);
      expect(control != nullptr, "the disposable font control should load");
      if (control == nullptr)
        continue;
      if (types[index] == ODT_LISTBOX || types[index] == ODT_COMBOBOX) {
        SendMessageW(control, types[index] == ODT_LISTBOX ? LB_ADDSTRING : CB_ADDSTRING,
                     0, reinterpret_cast<LPARAM>(L"Control-local font"));
      }
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(small), FALSE);
      EditorWindowTestAccess::set_owner_font(editor, large);
      const auto different_owner = render(editor, dialog, control, types[index], canvas);
      EditorWindowTestAccess::set_owner_font(editor, small);
      const auto matching_owner = render(editor, dialog, control, types[index], canvas);
      expect(different_owner == matching_owner,
             "button/checkbox/radio/list/combo drawing must ignore a different owner font");
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(large), FALSE);
      const auto changed_control = render(editor, dialog, control, types[index], canvas);
      expect(changed_control != matching_owner,
             "drawing must actually use the rendered control's WM_GETFONT");

      EditorWindowTestAccess::set_owner_font(editor, small);
      expect(chrome.on_dpi_changed(192, 0),
             "the dialog should replace its font independently at 192 DPI");
      expect(EditorWindowTestAccess::owner_font(editor) == small &&
                 reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)) ==
                     chrome.font(),
             "dialog DPI replacement must update the control, not the owner font");
      const auto changed_dpi = render(editor, dialog, control, types[index], canvas);
      EditorWindowTestAccess::set_owner_font(editor, chrome.font());
      expect(changed_dpi == render(editor, dialog, control, types[index], canvas),
             "DPI-changed drawing must use the new dialog-local font");
      expect(changed_dpi != matching_owner,
             "independent dialog DPI replacement should change rendered text");
      DestroyWindow(control);
    }
    EditorWindowTestAccess::set_owner_font(editor, nullptr);
    DestroyWindow(dialog);
    chrome.on_destroy();
  } else {
    DestroyWindow(dialog);
  }
  if (small != nullptr)
    DeleteObject(small);
  if (large != nullptr)
    DeleteObject(large);
}
