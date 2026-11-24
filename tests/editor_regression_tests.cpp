#include "dialog_layout.hpp"
#include "editor_layout_rules.hpp"
#include "profile_edit.hpp"
#include "editor_resource.h"
#include "editor_live_edit_test_access.hpp"
#include "editor_window.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
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
    auto profile = document.profile();
    profile.game.executable_name = "fixture.exe";
    layerc::core::VisualProfile visual;
    visual.profile_id = profile.id;
    layerc::core::AutomationProfile automation;
    automation.profile_id = profile.id;
    document.start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh,
                         std::move(visual), std::move(automation));
    const auto outcome = document.edit_profile(
        [](auto &candidate) { candidate.name += " edited"; },
        layerc::editor::DocumentChangeKind::immediate);
    (void)editor.apply_edit(outcome, layerc::editor::DocumentChangeKind::immediate);
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
  static FakeLiveEditTransport &bind_drag(EditorWindow &editor,
                                          const HWND owner, const HWND list,
                                          const HWND preview, const HWND status,
                                          LiveEditSession::Clock clock) {
    editor.shell_.instance = GetModuleHandleW(nullptr);
    editor.shell_.window = owner;
    editor.panel_preview_.list = list;
    editor.panel_preview_.window = preview;
    editor.panel_preview_.controls_panel = 0;
    editor.shell_.status = status;
    auto &transport = LiveEditSessionTestAccess::install(editor.live_edit_, std::move(clock));
    transport.changed = false;
    LiveEditSessionTestAccess::authenticate(editor.live_edit_);
    LiveEditSessionTestAccess::begin_session(editor.live_edit_, preview, 42);
    return transport;
  }
  static void unbind_drag(EditorWindow &editor) {
    editor.end_edit_on_window({}, false);
    editor.shell_.window = nullptr;
    editor.shell_.status = nullptr;
    editor.panel_preview_.list = nullptr;
    editor.panel_preview_.window = nullptr;
  }
  static LiveEditSession &session(EditorWindow &editor) { return editor.live_edit_; }
  static void poll(EditorWindow &editor) { editor.poll_live_channel(); }
  static void begin_picker(EditorWindow &editor) { editor.begin_picker(); }
  static bool picking(const EditorWindow &editor) { return editor.task_picker_.picking(); }
  static void direct_drag_off(EditorWindow &editor) { editor.set_direct_drag_enabled(false); }
  static void receive_anchor(EditorWindow &editor,
                             const layerc::core::LiveMessage &message) {
    if (auto *transport = dynamic_cast<FakeLiveEditTransport *>(
            LiveEditSessionTestAccess::transport_of(editor.live_edit_)))
      transport->inbox.push_back(message);
    editor.poll_live_channel();
  }
  static layerc::core::Panel effective_panel(EditorWindow &editor) {
    return editor.preview_panel(editor.document_.profile().panels.front());
  }
  static RECT effective_rect(EditorWindow &editor) {
    return editor.bar_preview_rect(editor.document_.profile().panels.front(),
                                   RECT{0, 0, 400, 240});
  }
  static bool has_pending_anchor(EditorWindow &editor) {
    return editor.panel_preview_.pending_anchor.has_value();
  }
  static void pending_anchor(EditorWindow &editor, const double x,
                             const double y, const bool dragging = false) {
    if (dragging) {
      editor.panel_preview_.begin_drag({0, 0}, {0, 0});
      editor.panel_preview_.mark_drag_moved();
    }
    editor.panel_preview_.pending_anchor = PanelPreview::PendingAnchor{
        editor.document_.profile().panels.front().id, x, y};
  }
  static bool finish_anchor(EditorWindow &editor) {
    return editor.commit_pending_anchor(true);
  }
  static void preview_message(EditorWindow &editor, const UINT message) {
    (void)editor.handle_preview_message(editor.panel_preview_.window,
                                         message, 0, 0);
  }
  static void end_drag_session(EditorWindow &editor) {
    editor.end_edit_on_window({}, false);
  }
  static void clear_drag_session(EditorWindow &editor) {
    LiveEditSessionTestAccess::clear_session(editor.live_edit_);
  }
  static void begin_drag_session(EditorWindow &editor, const HWND preview) {
    LiveEditSessionTestAccess::begin_session(editor.live_edit_, preview, 42);
  }
  static layerc::core::ProfileRevision sent_revision(EditorWindow &editor) {
    return editor.live_edit_.revision();
  }
  static layerc::core::ProfileRevision peer_revision(EditorWindow &editor) {
    return editor.live_edit_.peer_revision();
  }
  static bool pending_live_profile(EditorWindow &editor) {
    return editor.live_edit_.live_profile_pending();
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
  explicit PromptAnswer(const bool cancel, std::function<void()> before_answer = {})
      : cancel_(cancel), before_answer_(std::move(before_answer)) {
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
          if (active_->before_answer_) active_->before_answer_();
          // Messages go only to this test's own modal prompt, not input/game
          // windows. No input synthesis, hooks, or production data are used.
          const int preferred = active_->cancel_ ? IDC_PROMPT_BUTTON3
                                                : IDC_PROMPT_BUTTON2;
          // The button's own style: a modal dialog shows itself only once its
          // queue is empty, so a timer that fires first sees no visible parent.
          const auto style = GetWindowLongPtrW(GetDlgItem(window, preferred), GWL_STYLE);
          const int id = (style & WS_VISIBLE) != 0 ? preferred : IDC_PROMPT_BUTTON1;
          SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
          return FALSE;
        },
        0);
  }

  static inline PromptAnswer *active_{};
  UINT_PTR timer_{};
  bool cancel_{};
  int count_{};
  std::function<void()> before_answer_;
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

// The default 1196x753 editor has a 287 px timing column at 96 DPI. As in the
// editor shell, Capture key stays beside a 95 px binding; only the advanced
// HID button, which would overlap the field, wraps both below it.
void editor_binding_row_contracts() {
  using layerc::editor::binding_field_floor;
  using layerc::editor::capture_buttons_wrap;
  constexpr int label = 84;
  constexpr int gap = 8;
  constexpr int capture = 100;
  constexpr int advanced = capture + 60 + gap;
  expect(!capture_buttons_wrap(287, label, capture, gap, binding_field_floor),
         "the default timing column keeps Capture key on the binding row");
  expect(capture_buttons_wrap(287, label, advanced, gap, binding_field_floor),
         "Capture key and HID wrap where they would overlap the binding");
  expect(!capture_buttons_wrap(label + capture + gap + binding_field_floor,
                               label, capture, gap, binding_field_floor) &&
             capture_buttons_wrap(label + capture + gap + binding_field_floor -
                                      1,
                                  label, capture, gap, binding_field_floor),
         "the row wraps exactly when the binding would drop below its floor");
  for (const int dpi : {96, 144, 192}) {
    const auto scaled = [dpi](const int value) { return value * dpi / 96; };
    expect(!capture_buttons_wrap(scaled(287), scaled(label), scaled(capture),
                                 scaled(gap), scaled(binding_field_floor)),
           "the binding row decision is independent of DPI scaling");
  }
}

namespace {

// Own only hidden windows and a value-owned document. Never initialize the
// editor, connect a channel, open settings/stores, or synthesize input.
class DragFixture final {
public:
  DragFixture() : editor_(std::make_unique<layerc::editor::EditorWindow>()) {
    const auto instance = GetModuleHandleW(nullptr);
    owner_ = CreateWindowExW(0, L"STATIC", L"Layer51 drag fixture",
                              WS_OVERLAPPED, 0, 0, 500, 400, nullptr, nullptr,
                              instance, nullptr);
    list_ = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | LBS_HASSTRINGS,
                             0, 0, 100, 100, owner_, nullptr, instance, nullptr);
    preview_ = CreateWindowExW(0, L"STATIC", L"Preview", WS_CHILD,
                                100, 0, 400, 300, owner_, nullptr,
                                instance, nullptr);
    status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 300, 500, 24,
                              owner_, nullptr, instance, nullptr);
    if (!expect(owner_ != nullptr && list_ != nullptr && preview_ != nullptr && status_ != nullptr,
                "drag fixture owns its hidden shell/list/preview/status"))
      return;
    SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ability"));
    SendMessageW(list_, LB_SETITEMDATA, 0, 0);
    SendMessageW(list_, LB_SETCURSEL, 0, 0);
    auto profile = layerc::editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    profile.cooldowns.resize(1);
    profile.cooldowns.front().id = "ability";
    profile.cooldowns.front().name = "Ability";
    profile.cooldowns.front().duration = std::chrono::seconds(1);
    profile.panels.resize(1);
    auto &panel = profile.panels.front();
    panel.style = layerc::core::PanelStyle::bar;
    panel.layout = layerc::core::PanelLayout::row;
    panel.id = "panel";
    panel.name = "Ability";
    panel.anchor_x = 0.4;
    panel.anchor_y = 0.6;
    panel.cooldown_ids = {profile.cooldowns.front().id};
    profile.bindings.clear();
    // Core validation requires at least one binding: give the fixture cooldown
    // its own keyboard token so no old cooldown id survives the replacement.
    profile.bindings.push_back(
        {.cooldown_id = "ability",
         .chord = layerc::core::InputChord{
             {{layerc::core::InputDevice::keyboard, 0x12, 0}}}});
    profile.hid_bindings.clear();
    profile.cooldown_groups.clear();
    if (const auto profile_error = layerc::core::validate_profile(profile)) {
      expect(false, "drag fixture is a valid complete profile: " +
                        profile_error->describe());
      return;
    }
    layerc::core::VisualProfile visual;
    visual.profile_id = profile.id;
    layerc::core::AutomationProfile automation;
    automation.profile_id = profile.id;
    document().start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh,
                           std::move(visual), std::move(automation));
    transport_ = &EditorWindowTestAccess::bind_drag(*editor_, owner_, list_, preview_,
                                                   status_, [this] { return now; });
    ready_ = true;
  }
  ~DragFixture() {
    if (ready_) EditorWindowTestAccess::unbind_drag(*editor_);
    if (owner_ != nullptr) DestroyWindow(owner_);
  }
  DragFixture(const DragFixture &) = delete;
  DragFixture &operator=(const DragFixture &) = delete;
  bool ready() const noexcept { return ready_; }
  layerc::editor::EditorWindow &editor() { return *editor_; }
  layerc::editor::ProfileDocument &document() {
    return EditorWindowTestAccess::document(*editor_);
  }
  HWND owner() const noexcept { return owner_; }
  HWND preview() const noexcept { return preview_; }
  HWND other_owned_child() const noexcept { return list_; }
  layerc::editor::FakeLiveEditTransport &transport() { return *transport_; }
  std::wstring status() const {
    wchar_t buffer[512]{};
    GetWindowTextW(status_, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
  }
  layerc::editor::EditorProfileSnapshot current() { return snapshot(document()); }
  ULONGLONG now{1000};
private:
  std::unique_ptr<layerc::editor::EditorWindow> editor_;
  HWND owner_{};
  HWND list_{};
  HWND preview_{};
  HWND status_{};
  layerc::editor::FakeLiveEditTransport *transport_{};
  bool ready_{};
};

void pick_ends_with_on_window_edit() {
  using layerc::core::LiveMessageType;
  using layerc::editor::LiveEditEndReason;
  using layerc::editor::LiveEditSession;
  for (const auto reason : {LiveEditEndReason::connection_lost,
                            LiveEditEndReason::daemon_ended,
                            LiveEditEndReason::direct_drag_unconfirmed,
                            LiveEditEndReason::daemon_error,
                            LiveEditEndReason::target_closed}) {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    auto &session = EditorWindowTestAccess::session(editor);
    EditorWindowTestAccess::begin_picker(editor);
    if (!expect(EditorWindowTestAccess::picking(editor) && GetCapture() == fixture.owner(),
                "Pick holds capture before the edit ends")) return;
    switch (reason) {
    case LiveEditEndReason::connection_lost:
      fixture.transport().online = false;
      fixture.transport().changed = true;
      break;
    case LiveEditEndReason::daemon_ended:
      fixture.transport().inbox.emplace_back(layerc::core::LiveMessage{
          .type = LiveMessageType::end_edit, .edit_session = session.session_id()});
      break;
    case LiveEditEndReason::direct_drag_unconfirmed:
      expect(session.send_direct_drag(false), "the fixture queues an off without a reply");
      fixture.now += LiveEditSession::direct_drag_ack_ms;
      break;
    case LiveEditEndReason::daemon_error:
      fixture.transport().inbox.emplace_back(layerc::core::LiveMessage{
          .type = LiveMessageType::error, .edit_session = session.session_id(),
          .text = "unknown panel id"});
      break;
    case LiveEditEndReason::target_closed:
      EditorWindowTestAccess::begin_drag_session(editor, nullptr);
      fixture.now += LiveEditSession::heartbeat_interval_ms;
      break;
    }
    EditorWindowTestAccess::poll(editor);
    expect(!session.editing() && !EditorWindowTestAccess::picking(editor) && GetCapture() == nullptr,
           "polling an end of edit cancels Pick and releases shell capture");
    if (reason == LiveEditEndReason::connection_lost)
      expect(fixture.status() == L"On-window editing ended — the LayerD connection closed.",
             "the connection-lost status does not claim the overlays are restored");
    if (reason == LiveEditEndReason::direct_drag_unconfirmed)
      expect(fixture.status() == L"On-window editing ended — LayerD did not confirm Direct drag off. "
                                 L"Pick window waits until LayerD confirms its panels are click-through.",
             "the unconfirmed status says Pick waits for proof");
    if (EditorWindowTestAccess::picking(editor)) EditorWindowTestAccess::begin_picker(editor);
  }
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    auto &session = EditorWindowTestAccess::session(editor);
    (void)session.end(false);
    fixture.transport().online = false;
    fixture.transport().changed = true;
    EditorWindowTestAccess::poll(editor);
    expect(session.request_begin(fixture.preview(), {}, 1) ==
               layerc::editor::LiveEditBegin::awaiting_daemon,
           "the pending edit waits without launching a daemon");
    EditorWindowTestAccess::begin_picker(editor);
    if (!expect(EditorWindowTestAccess::picking(editor) && GetCapture() == fixture.owner(),
                "Pick holds capture before the pending edit expires")) return;
    ++fixture.now;
    const PromptAnswer answer(false, [&] {
      expect(!EditorWindowTestAccess::picking(editor) && GetCapture() != fixture.owner(),
             "pending edit expiry cancels Pick before answering the modal prompt");
    });
    EditorWindowTestAccess::poll(editor);
    expect(answer.count() == 1 && !EditorWindowTestAccess::picking(editor) && GetCapture() == nullptr,
           "pending edit expiry releases capture and shows its prompt once");
  }
}

void pick_refused_until_layerd_release() {
  DragFixture fixture;
  if (!fixture.ready()) return;
  auto &editor = fixture.editor();
  auto &session = EditorWindowTestAccess::session(editor);
  expect(session.send_direct_drag(true), "the fake accepts Direct drag on");
  fixture.transport().accept_send = false;
  EditorWindowTestAccess::direct_drag_off(editor);
  expect(!session.editing() && session.release_pending() &&
             fixture.status() == L"On-window editing ended — Direct drag off could not reach LayerD. "
                                 L"Pick window waits until LayerD confirms its panels are click-through.",
         "an unsent off ends editing and its status says Pick waits for proof");
  fixture.now += 10000;
  EditorWindowTestAccess::begin_picker(editor);
  expect(!EditorWindowTestAccess::picking(editor) && GetCapture() == nullptr &&
             fixture.status() == L"Pick window unavailable — LayerD has not confirmed its panels "
                                 L"are click-through. Choose the window from the list, or try again.",
         "Pick refusal keeps capture free and gives the window-list alternative while release is pending");
}

layerc::core::LiveMessage anchor_message(
    const layerc::core::ProfileRevision revision, const double x, const double y,
    const bool final = false,
    const layerc::core::EditSessionId session = 42) {
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::anchor_changed;
  message.revision = revision;
  message.edit_session = session;
  message.panel_id = "panel";
  message.anchor_x = x;
  message.anchor_y = y;
  message.final = final;
  return message;
}

} // namespace

void editor_drag_transaction_contracts() {
  INITCOMMONCONTROLSEX controls{.dwSize = sizeof(controls),
                                .dwICC = ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  pick_ends_with_on_window_edit();
  pick_refused_until_layerd_release();
  // 1. A remote non-final is a visible projection, never a document edit.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    const auto original_rect = EditorWindowTestAccess::effective_rect(editor);
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(1, 0.8, 0.2));
    const auto projected = EditorWindowTestAccess::effective_panel(editor);
    const auto moved_rect = EditorWindowTestAccess::effective_rect(editor);
    expect(fixture.current() == before && !fixture.document().dirty() &&
               !fixture.document().can_undo(),
           "drag1 non-final preserves complete profile/modules/history");
    expect(projected.anchor_x == 0.8 && projected.anchor_y == 0.2 &&
               moved_rect.left > original_rect.left &&
               moved_rect.top < original_rect.top,
           "drag1 preview geometry follows pending anchor, not stored anchor");
  }
  // 2. Remote final, duplicate final and capture loss yield exactly one edit.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(1, 0.8, 0.2));
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(2, 0.75, 0.25, true));
    const auto final = fixture.current();
    expect(final.profile.panels.front().anchor_x == 0.75 &&
               final.profile.panels.front().anchor_y == 0.25 &&
               fixture.document().dirty() && fixture.document().can_undo() &&
               !EditorWindowTestAccess::has_pending_anchor(editor),
           "drag2 remote final accepts one transaction and clears pending");
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(2, 0.75, 0.25, true));
    EditorWindowTestAccess::preview_message(editor, WM_CAPTURECHANGED);
    expect(EditorWindowTestAccess::sent_revision(editor) == 0,
           "drag2 incoming final is never echoed as an outgoing final anchor");
    EditorWindowTestAccess::undo(editor);
    expect(fixture.current() == before && !fixture.document().can_undo(),
           "drag2 one undo restores exact baseline after repeated final/capture loss");
    EditorWindowTestAccess::redo(editor);
    expect(fixture.current() == final && !fixture.document().can_redo(),
           "drag2 one redo restores exact final profile and modules");
  }
  // 3. Local final then capture-change/button-up must not double-record.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    EditorWindowTestAccess::pending_anchor(editor, 0.65, 0.35, true);
    expect(EditorWindowTestAccess::finish_anchor(editor),
           "drag3 changed local final is accepted");
    EditorWindowTestAccess::preview_message(editor, WM_CAPTURECHANGED);
    EditorWindowTestAccess::preview_message(editor, WM_LBUTTONUP);
    expect(fixture.document().profile().panels.front().anchor_x == 0.65 &&
               fixture.document().profile().panels.front().anchor_y == 0.35,
           "drag3 local coordinates become committed only at final");
    EditorWindowTestAccess::undo(editor);
    expect(fixture.current() == before && !fixture.document().can_undo(),
           "drag3 local final/capture loss/button up create one undo step");
  }
  // 4. End/new discard projection and release only this preview's capture.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    EditorWindowTestAccess::pending_anchor(editor, 0.9, 0.1, true);
    if (expect(GetCapture() == nullptr,
               "drag4 fixture starts without another current-thread capture")) {
      SetCapture(fixture.preview());
      expect(GetCapture() == fixture.preview(), "drag4 owns preview capture");
      EditorWindowTestAccess::end_drag_session(editor);
      expect(GetCapture() == nullptr, "drag4 ending releases its preview capture");
    } else {
      EditorWindowTestAccess::end_drag_session(editor);
    }
    expect(!EditorWindowTestAccess::has_pending_anchor(editor) &&
               fixture.current() == before && !fixture.document().can_undo(),
           "drag4 ending discards rather than commits pending coordinates");
    EditorWindowTestAccess::begin_drag_session(editor, fixture.preview());
    EditorWindowTestAccess::pending_anchor(editor, 0.95, 0.05);
    if (GetCapture() == nullptr) {
      SetCapture(fixture.other_owned_child());
      EditorWindowTestAccess::end_drag_session(editor);
      expect(GetCapture() == fixture.other_owned_child(),
             "drag4 ending does not release a different owned child's capture");
      if (GetCapture() == fixture.other_owned_child()) ReleaseCapture();
    }
    EditorWindowTestAccess::pending_anchor(editor, 0.9, 0.1);
    EditorWindowTestAccess::new_profile(editor, false);
    expect(!EditorWindowTestAccess::has_pending_anchor(editor) &&
               !fixture.document().can_undo() &&
               fixture.document().profile().panels.front().anchor_x == 0.5 &&
               fixture.document().profile().panels.front().anchor_y == 0.75,
           "drag4 new document cannot inherit previous panel id/coordinates");
  }
  // 5. Invalid messages neither replace a valid projection nor poison revision.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    EditorWindowTestAccess::clear_drag_session(editor);
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(1, 0.8, 0.2, false, 0));
    expect(!EditorWindowTestAccess::has_pending_anchor(editor) && fixture.current() == before,
           "drag5 zero session cannot accept anchor messages");
    EditorWindowTestAccess::begin_drag_session(editor, fixture.preview());
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(1, 0.8, 0.2));
    std::vector<layerc::core::LiveMessage> invalid{
        anchor_message(2, 0.5, 0.5, false, 7),
        anchor_message(1, 0.5, 0.5),
        anchor_message(100, std::numeric_limits<double>::quiet_NaN(), 0.5),
        anchor_message(100, 0.5, std::numeric_limits<double>::infinity()),
        anchor_message(100, -0.1, 0.5),
        anchor_message(100, 0.5, 1.1)};
    auto missing = anchor_message(100, 0.5, 0.5);
    missing.panel_id = "missing";
    invalid.push_back(std::move(missing));
    for (const auto &message : invalid) {
      EditorWindowTestAccess::receive_anchor(editor, message);
      const auto projected = EditorWindowTestAccess::effective_panel(editor);
      expect(fixture.current() == before && !fixture.document().dirty() &&
                 !fixture.document().can_undo() && projected.anchor_x == 0.8 &&
                 projected.anchor_y == 0.2 &&
                 EditorWindowTestAccess::peer_revision(editor) == 1,
             "drag5 invalid/stale input preserves valid pending snapshot/revision");
    }
    EditorWindowTestAccess::receive_anchor(editor, anchor_message(2, 0.7, 0.3));
    const auto projected = EditorWindowTestAccess::effective_panel(editor);
    expect(projected.anchor_x == 0.7 && projected.anchor_y == 0.3 &&
               EditorWindowTestAccess::peer_revision(editor) == 2 &&
               fixture.current() == before,
           "drag5 invalid high revisions cannot poison next valid lower revision");
  }
  // 6. Returning to the original coordinate is no history or publish event.
  {
    DragFixture fixture;
    if (!fixture.ready()) return;
    auto &editor = fixture.editor();
    const auto before = fixture.current();
    const auto revision = EditorWindowTestAccess::sent_revision(editor);
    const auto live_pending = EditorWindowTestAccess::pending_live_profile(editor);
    EditorWindowTestAccess::pending_anchor(editor, 0.4, 0.6, true);
    expect(!EditorWindowTestAccess::finish_anchor(editor),
           "drag6 unchanged gesture is not an accepted change");
    EditorWindowTestAccess::preview_message(editor, WM_CAPTURECHANGED);
    expect(fixture.current() == before && !fixture.document().dirty() &&
               !fixture.document().can_undo() &&
               !EditorWindowTestAccess::has_pending_anchor(editor) &&
               EditorWindowTestAccess::sent_revision(editor) == revision &&
               EditorWindowTestAccess::pending_live_profile(editor) == live_pending,
           "drag6 unchanged gesture records/publishes/notifies nothing");
  }
}
