#include "automation_editor.hpp"
#include "automation_graph.hpp"
#include "cooldown_editor.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_profile_fields.hpp"
#include "editor_text.hpp"
#include "editor_util.hpp"
#include "profile_document.hpp"
#include "test_support.hpp"
#include "visual_editor.hpp"

#include "editor_resource.h"
#include "layerc/platform/text.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// The cooldown and visual sub-editors are components: each is driven here
// through its injected document, shell, and host callbacks alone, without
// an EditorWindow.
namespace layerc::editor {

// Focused seam for the automation editor's private reporting paths; no public
// production test API is added.
struct AutomationEditorTestAccess {
  // The hidden dialog through the production dispatch, so WM_SIZE,
  // WM_GETMINMAXINFO and WM_DESTROY reach the editor as they do in open().
  static HWND open_modeless(AutomationEditor &editor) {
    return CreateDialogParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_AUTOMATION), nullptr,
        &dispatch_member_dialog<AutomationEditor,
                                &AutomationEditor::initialize,
                                &AutomationEditor::handle_message>,
        reinterpret_cast<LPARAM>(&editor));
  }
  static SIZE minimum_client(const AutomationEditor &editor) {
    return editor.minimum_client_;
  }
  static void refresh(AutomationEditor &editor, const HWND dialog) {
    editor.refresh(dialog);
  }
  static bool apply_json(AutomationEditor &editor, const HWND dialog) {
    return editor.apply_json(dialog);
  }
  static bool command(AutomationEditor &editor, const HWND dialog,
                      const WPARAM wparam) {
    return editor.handle_command(dialog, wparam);
  }
  static bool structural(AutomationEditor &editor, const HWND dialog) {
    return editor.mutate_automation(
               dialog, L"Fixture automation edit",
               [](std::optional<layerc::core::AutomationProfile> &automation)
                   -> std::optional<std::string> {
                 layerc::core::AutomationDefinition added;
                 added.id = "automation-2";
                 added.name = "Second";
                 automation->automations.push_back(std::move(added));
                 return std::nullopt;
               }) == AutomationEditor::Mutation::committed;
  }
};

} // namespace layerc::editor

namespace {
using namespace layerc::core;
using layerc::editor::CooldownEditor;
using layerc::editor::DocumentChangeKind;
using layerc::editor::ProfileDocument;
using layerc::editor::VisualEditor;
using layerc::test::expect;

class NullTheme final : public layerc::editor::DialogTheme {
public:
  void draw_list_box_item(const DRAWITEMSTRUCT &) const override {}
  [[nodiscard]] std::optional<LRESULT>
  dialog_theme_message(HWND, UINT, WPARAM, LPARAM) const override {
    return std::nullopt;
  }
  void style_dialog_children(HWND) const override {}
};

class ListBox final {
public:
  ListBox()
      : window_(CreateWindowExW(0, L"LISTBOX", L"", WS_POPUP | LBS_NOTIFY, 0,
                                0, 200, 200, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr)) {}
  ~ListBox() {
    if (window_ != nullptr) DestroyWindow(window_);
  }
  ListBox(const ListBox &) = delete;
  ListBox &operator=(const ListBox &) = delete;
  [[nodiscard]] HWND get() const noexcept { return window_; }

private:
  HWND window_;
};

struct HostCalls {
  int sync_fields{};
  int follow_bar{};
  int refresh_visual{};
  int refresh_bars{};
  std::size_t preferred_panel{SIZE_MAX};
  int immediate_changes{};
};

MSG key_down(const WPARAM virtual_key, const std::uint16_t scan_code) {
  MSG message{};
  message.message = WM_KEYDOWN;
  message.wParam = virtual_key;
  message.lParam = static_cast<LPARAM>(scan_code) << 16;
  return message;
}

// Answers only this thread's own modal prompt; no input synthesis or global
// window access is involved.
class PromptYes final {
public:
  PromptYes() {
    active_ = this;
    timer_ = SetTimer(nullptr, 0, 10, &on_timer);
  }
  ~PromptYes() {
    if (timer_ != 0) KillTimer(nullptr, timer_);
    active_ = nullptr;
  }
  [[nodiscard]] bool answered() const noexcept { return answered_; }
private:
  static void CALLBACK on_timer(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](const HWND window, LPARAM) -> BOOL {
          if (active_ == nullptr ||
              GetDlgItem(window, IDC_PROMPT_TEXT) == nullptr)
            return TRUE;
          active_->answered_ = true;
          SendMessageW(window, WM_COMMAND,
                       MAKEWPARAM(IDC_PROMPT_BUTTON1, BN_CLICKED), 0);
          return FALSE;
        },
        0);
  }
  static inline PromptYes *active_{};
  UINT_PTR timer_{};
  bool answered_{};
};

const InputBinding *binding_for(const GameProfile &profile,
                                const std::string &cooldown_id) {
  for (const auto &binding : profile.bindings)
    if (binding.cooldown_id == cooldown_id) return &binding;
  return nullptr;
}



} // namespace

void editor_cooldown_component_contracts() {
  ProfileDocument document;
  // F01: a draft is created through the document transition, and an editable
  // profile needs an application matcher for validated edits to be accepted.
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "component.exe";
  document.start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh);
  layerc::editor::EditorShell shell;
  const NullTheme theme;
  HostCalls calls;
  CooldownEditor editor(
      document, shell, theme,
      [&](const layerc::editor::ModuleEditOutcome &outcome,
          const DocumentChangeKind kind) {
        if (outcome.accepted() && kind == DocumentChangeKind::immediate)
          ++calls.immediate_changes;
        return outcome.accepted() ||
               outcome.status == layerc::editor::ModuleEditStatus::unchanged;
      },
      {.sync_fields = [&] { ++calls.sync_fields; },
       .follow_bar = [&] { ++calls.follow_bar; },
       .refresh_visual = [&] { ++calls.refresh_visual; },
       .refresh_bars =
           [&](const std::size_t preferred) {
             ++calls.refresh_bars;
             calls.preferred_panel = preferred;
           }});
  const ListBox list;
  expect(list.get() != nullptr, "the cooldown list fixture is created");
  editor.bind(list.get());

  editor.refresh_list();
  expect(SendMessageW(list.get(), LB_GETCOUNT, 0, 0) ==
             static_cast<LRESULT>(document.profile().cooldowns.size()),
         "the component lists every cooldown of the injected document");
  expect(editor.selected() == &document.profile().cooldowns.front(),
         "the first cooldown is selected");
  expect(calls.follow_bar == 1 && calls.refresh_visual == 1,
         "a refresh projects the selection through the host callbacks");

  const auto before = document.profile().cooldowns.size();
  editor.add();
  const auto added = document.profile().cooldowns.back();
  expect(document.profile().cooldowns.size() == before + 1U &&
             editor.selected() != nullptr && editor.selected()->id == added.id,
         "add appends and selects a cooldown");
  expect(calls.sync_fields == 1 && calls.immediate_changes == 1,
         "add commits pending fields first and records one change");
  expect(calls.refresh_bars == 1 &&
             calls.preferred_panel == document.profile().panels.size() - 1U,
         "add asks the host to show the new cooldown's bar");
  const auto *initial = binding_for(document.profile(), added.id);
  expect(initial != nullptr && initial->chord.tokens.size() == 1U &&
             initial->chord.tokens.front().device == InputDevice::keyboard,
         "an added cooldown starts with an unused keyboard binding");

  editor.begin_capture();
  expect(editor.capturing_binding(), "capture starts for a selection");
  expect(editor.handle_capture_message(key_down('Q', 0x10)),
         "capture consumes the chord's key");
  const auto *captured = binding_for(document.profile(), added.id);
  expect(!editor.capturing_binding() && captured != nullptr &&
             captured->chord.tokens.size() == 1U &&
             captured->chord.tokens.front().code == 0x10U,
         "a captured key replaces the selected cooldown's binding");
  expect(calls.immediate_changes == 2, "a captured binding is one change");

  editor.begin_capture();
  expect(editor.handle_capture_message(key_down(VK_ESCAPE, 0x01)) &&
             !editor.capturing_binding(),
         "Esc cancels capture");
  expect(calls.immediate_changes == 2 &&
             binding_for(document.profile(), added.id)->chord.tokens.front()
                     .code == 0x10U,
         "a cancelled capture changes nothing");

  // 1. An unchanged chord capture ends capture without reporting a change.
  editor.begin_capture();
  expect(editor.handle_capture_message(key_down('Q', 0x10)) &&
             !editor.capturing_binding(),
         "recapturing the same chord ends capture");
  expect(calls.immediate_changes == 2,
         "an unchanged chord reports no accepted change");

  // 2. Duplicate is exactly one undoable step and copies the source fields.
  const auto before_duplicate = document.profile();
  document.start_draft(before_duplicate, layerc::editor::DraftOrigin::fresh);
  editor.refresh_list(static_cast<int>(before_duplicate.cooldowns.size()) - 1);
  const auto *duplicate_source = editor.selected();
  if (!expect(duplicate_source != nullptr, "duplicate needs a selection")) return;
  const auto source_value = *duplicate_source;
  const int changes_before_duplicate = calls.immediate_changes;
  editor.duplicate();
  expect(calls.immediate_changes == changes_before_duplicate + 1,
         "duplicate reports one accepted change");
  const auto after_duplicate = document.profile();
  expect(after_duplicate.cooldowns.size() == before_duplicate.cooldowns.size() + 1U,
         "duplicate adds exactly one cooldown");
  const auto &copied = after_duplicate.cooldowns.back();
  expect(copied.name == source_value.name + " copy" &&
             copied.duration == source_value.duration &&
             copied.max_charges == source_value.max_charges &&
             copied.retrigger == source_value.retrigger &&
             copied.recovery == source_value.recovery,
         "duplicate copies the source name and timing fields");
  expect(document.undo() && document.profile() == before_duplicate &&
             !document.can_undo(),
         "one undo restores the pre-duplicate content");
  expect(document.redo() && document.profile() == after_duplicate &&
             !document.can_redo(),
         "one redo restores the duplicate");

  // 3. Removal notifies the host once and undoes in one step.
  document.start_draft(after_duplicate, layerc::editor::DraftOrigin::fresh);
  editor.refresh_list(static_cast<int>(after_duplicate.cooldowns.size()) - 1);
  const int changes_before_remove = calls.immediate_changes;
  {
    PromptYes prompt;
    editor.remove();
    expect(prompt.answered(), "the removal prompt is answered on this thread");
  }
  expect(calls.immediate_changes == changes_before_remove + 1,
         "removal notifies autosave/live exactly once");
  const auto after_remove = document.profile();
  expect(after_remove.cooldowns.size() == after_duplicate.cooldowns.size() - 1U,
         "removal drops exactly one cooldown");
  expect(document.undo() && document.profile() == after_duplicate &&
             !document.can_undo(),
         "one undo restores the removed cooldown");
  expect(document.redo() && document.profile() == after_remove,
         "redo restores the removal");

  SendMessageW(list.get(), LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
  editor.begin_capture();
  expect(editor.selected() == nullptr && !editor.capturing_binding(),
         "capture needs a selected cooldown");
  expect(!editor.handle_capture_message(key_down('Q', 0x10)),
         "keys pass through when capture is idle");
}

void editor_visual_component_contracts() {
  using layerc::editor::CommitRecord;
  using layerc::editor::CommitResult;
  using layerc::editor::EditorProfileSnapshot;
  using layerc::editor::ProfileBundle;
  ProfileDocument document;
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "component.exe";
  const auto cooldown_id = profile.cooldowns.front().id;
  VisualProfile visual;
  visual.profile_id = profile.id;
  visual.observations.push_back(
      PixelRule{.id = "ready", .name = "Ready", .region = {}, .predicate = {}});
  visual.triggers.push_back(
      {.cooldown_id = cooldown_id, .observation_id = "ready"});
  const std::filesystem::path path = L"C:\\fixture\\profile.json";
  document.accept_commit(path, ProfileBundle{.profile = std::move(profile),
                                              .visual = std::move(visual),
                                              .automation = {}});
  const auto snapshot = [&] {
    return EditorProfileSnapshot{document.profile(), document.visual(),
                                 document.automation()};
  };
  const auto initial = snapshot();
  expect(!document.dirty() && !document.can_undo(),
         "visual fixture starts saved via the owned document transition");

  layerc::editor::EditorShell shell;
  // LiveEditSession is about 64 KiB; keep it off the case's stack (C6262).
  const auto live_edit = std::make_unique<const layerc::editor::LiveEditSession>();
  layerc::editor::TaskPicker task_picker;
  auto commit = CommitResult::ok;
  int commits{};
  int committed{};
  int tasks_shown{};
  std::optional<VisualProfile> last_candidate;
  VisualEditor editor(
      document, shell, *live_edit, task_picker,
      {.selected_cooldown = [&]() -> const CooldownDefinition * {
         return &document.profile().cooldowns.front();
       },
       .commit_visual = [&](VisualProfile candidate) {
         ++commits;
         last_candidate = candidate;
         if (commit == CommitResult::ok) {
           auto bundle = document.bundle();
           bundle.visual = std::move(candidate);
           document.accept_commit(path, std::move(bundle), CommitRecord::record_edit);
         }
         return commit;
       },
       .mark_committed = [&] { ++committed; },
       .write_failure_message = [] { return std::wstring(L"fixture"); },
       .require_stored_profile = [](const wchar_t *) { return true; },
       .show_tasks = [&] { ++tasks_shown; }});

  editor.set_enabled(false);
  const auto disabled = snapshot();
  expect(last_candidate && !last_candidate->triggers.front().enabled &&
             !document.visual()->triggers.front().enabled && commits == 1 &&
             committed == 1 && !document.dirty() && document.can_undo(),
         "visual disable commits one detached candidate and one notification");
  editor.set_enabled(false);
  expect(commits == 1 && committed == 1 && snapshot() == disabled,
         "already-disabled visual is no commit, notification or edit");
  expect(document.undo() && snapshot() == initial && document.dirty() &&
             !document.can_undo(),
         "one undo restores enabled visual and exact modules, dirty against saved disable");
  expect(document.redo() && snapshot() == disabled && !document.dirty() &&
             !document.can_redo(),
         "one redo restores exact saved disabled bundle without a second history step");

  commit = CommitResult::upgrade_declined;
  const auto before_refusal = snapshot();
  const auto before_path = document.path();
  const auto *profile_address = std::addressof(document.profile());
  const auto *visual_storage = std::addressof(document.visual());
  const auto *trigger_address = std::addressof(document.visual()->triggers.front());
  const bool before_dirty = document.dirty();
  const bool before_undo = document.can_undo();
  const bool before_redo = document.can_redo();
  editor.set_enabled(true);
  expect(last_candidate && last_candidate->triggers.front().enabled && commits == 2 &&
             committed == 1 && snapshot() == before_refusal &&
             document.path() == before_path && document.dirty() == before_dirty &&
             document.can_undo() == before_undo && document.can_redo() == before_redo &&
             std::addressof(document.profile()) == profile_address &&
             std::addressof(document.visual()) == visual_storage &&
             std::addressof(document.visual()->triggers.front()) == trigger_address,
         "declined visual candidate preserves saved values, path, storage and history");

  expect(document.edit_profile([](auto &candidate) {
           candidate.name += " pending";
         }, DocumentChangeKind::immediate).accepted(),
         "a distinct pending base edit is accepted before the second refusal");
  const auto dirty_snapshot = snapshot();
  const auto *dirty_profile_address = std::addressof(document.profile());
  const auto *dirty_trigger_address = std::addressof(document.visual()->triggers.front());
  const auto dirty_undo = document.can_undo();
  const auto dirty_redo = document.can_redo();
  editor.set_enabled(true);
  expect(commits == 3 && committed == 1 && snapshot() == dirty_snapshot &&
             document.dirty() && document.path() == before_path &&
             document.can_undo() == dirty_undo && document.can_redo() == dirty_redo &&
             std::addressof(document.profile()) == dirty_profile_address &&
             std::addressof(document.visual()) == visual_storage &&
             std::addressof(document.visual()->triggers.front()) == dirty_trigger_address,
         "repeated declined visual candidate preserves dirty edits without rollback replacement");
  editor.set_enabled(false);
  expect(commits == 3 && committed == 1 && snapshot() == dirty_snapshot && document.dirty(),
         "visual no-op does not save or clear an unrelated pending base edit");
  editor.calibrate();
  expect(task_picker.awaiting_calibration() && tasks_shown == 1,
         "without a live target calibration asks the host for a task");
}

// F01 B2a: the automation editor reports through the one funnel. No daemon,
// store, settings or live IPC is touched.
void editor_automation_component_contracts() {
  using namespace layerc::editor;
  auto profile = make_new_profile();
  profile.game.executable_name = "component.exe";
  layerc::core::AutomationProfile baseline;
  baseline.profile_id = profile.id;
  layerc::core::AutomationDefinition first;
  first.id = "automation-1";
  first.name = "First";
  first.enabled = true;
  baseline.automations.push_back(first);
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh, std::nullopt, baseline);
  const auto live = std::make_unique<LiveEditSession>();
  EditorShell shell;
  const NullTheme theme;
  int immediate{};
  int text{};
  AutomationEditor editor(
      document, *live, shell, theme,
      [&](const ModuleEditOutcome &outcome, const DocumentChangeKind kind) {
        if (outcome.accepted()) {
          if (kind == DocumentChangeKind::text)
            ++text;
          else
            ++immediate;
        }
        return outcome.accepted() ||
               outcome.status == ModuleEditStatus::unchanged;
      });
  struct DialogGuard final {
    HWND handle = CreateDialogParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_AUTOMATION), nullptr,
        [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR { return FALSE; }, 0);
    ~DialogGuard() { if (handle != nullptr) DestroyWindow(handle); }
  } dialog;
  if (!expect(dialog.handle != nullptr, "the hidden automation resource loads")) return;

  // 1. A structural edit is one undo step.
  AutomationEditorTestAccess::refresh(editor, dialog.handle);
  expect(AutomationEditorTestAccess::structural(editor, dialog.handle) &&
             immediate == 1 && document.automation()->automations.size() == 2U &&
             document.dirty() && document.can_undo(),
         "an accepted structural edit reports once and is one undo step");
  const auto after_structural = document.automation();
  expect(document.undo() && document.automation() == baseline &&
             !document.can_undo(),
         "one undo restores the baseline automation graph");
  expect(document.redo() && document.automation() == after_structural &&
             !document.can_redo(),
         "one redo restores the structural edit");

  // 2. JSON apply records with the immediate kind (regression for the missing
  //    DocumentChangeKind at the closure).
  document.start_draft(profile, DraftOrigin::fresh, std::nullopt, baseline);
  AutomationEditorTestAccess::refresh(editor, dialog.handle);
  auto json_candidate = baseline;
  json_candidate.automations.front().name = "JSON changed";
  const auto json_text = layerc::platform::utf8_to_wide(automation_graph_json(
      std::optional<layerc::core::AutomationProfile>{json_candidate}));
  SetDlgItemTextW(dialog.handle, IDC_AUTOMATION_JSON, json_text.c_str());
  expect(AutomationEditorTestAccess::apply_json(editor, dialog.handle) &&
             immediate == 2 && document.automation() == json_candidate &&
             document.dirty() && document.can_undo(),
         "an accepted JSON edit reports exactly one immediate change");
  expect(AutomationEditorTestAccess::apply_json(editor, dialog.handle) &&
             immediate == 2,
         "an unchanged JSON apply reports no accepted change");
  expect(document.undo() && document.automation() == baseline &&
             !document.can_undo(),
         "one undo restores the pre-JSON graph");

  // 3. A name edit reports once as a text change.
  document.start_draft(profile, DraftOrigin::fresh, std::nullopt, baseline);
  AutomationEditorTestAccess::refresh(editor, dialog.handle);
  SetDlgItemTextW(dialog.handle, IDC_AUTOMATION_NAME, L"Name changed");
  expect(AutomationEditorTestAccess::command(
             editor, dialog.handle,
             MAKEWPARAM(IDC_AUTOMATION_NAME, EN_KILLFOCUS)) &&
             text == 1 && document.automation()->automations.front().name == "Name changed" &&
             document.dirty() && document.can_undo(),
         "an accepted name edit reports one text change");
  expect(AutomationEditorTestAccess::command(
             editor, dialog.handle,
             MAKEWPARAM(IDC_AUTOMATION_NAME, EN_KILLFOCUS)) &&
             text == 1,
         "an unchanged name reports no further change");
  expect(document.undo() && document.automation() == baseline &&
             !document.can_undo(),
         "one undo restores the baseline name");
}

// Review GUI-03: at the size Automation reports as its minimum, each view's
// visible controls fit its client area without overlapping, and a window
// made smaller grows back to that size. Review GUI-04: the simulator speed
// list is named by the visible label just before it (MSAA takes a list's
// name from that static), with an access key unique in the dialog.
void editor_automation_layout_contracts() {
  using namespace layerc::editor;
  auto profile = make_new_profile();
  profile.game.executable_name = "component.exe";
  layerc::core::AutomationProfile automation;
  automation.profile_id = profile.id;
  layerc::core::AutomationDefinition first;
  first.id = "automation-1";
  first.name = "First";
  automation.automations.push_back(first);
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh, std::nullopt, automation);
  const auto live = std::make_unique<LiveEditSession>();
  EditorShell shell;
  const NullTheme theme;
  AutomationEditor editor(
      document, *live, shell, theme,
      [](const ModuleEditOutcome &, DocumentChangeKind) { return true; });
  // Declared after the editor, so the dialog (and its WM_DESTROY) goes first.
  struct DialogGuard final {
    HWND handle{};
    ~DialogGuard() { if (handle != nullptr) DestroyWindow(handle); }
  } dialog{AutomationEditorTestAccess::open_modeless(editor)};
  if (!expect(dialog.handle != nullptr,
              "the automation dialog opens through its dispatch"))
    return;
  const HWND window = dialog.handle;
  const auto visible = [](const HWND control) {
    return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
  };
  const auto client_rect = [window](const HWND control) {
    RECT rect{};
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&rect), 2);
    return rect;
  };
  const auto resize = [window](const LONG width, const LONG height) {
    SetWindowPos(window, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  };
  constexpr std::array<const char *, 3> view_names{"Basic", "Simulator",
                                                   "Advanced"};
  for (int view = 0; view < static_cast<int>(view_names.size()); ++view) {
    const std::string name = view_names[static_cast<std::size_t>(view)];
    SendDlgItemMessageW(window, IDC_AUTOMATION_VIEW, CB_SETCURSEL,
                        static_cast<WPARAM>(view), 0);
    (void)AutomationEditorTestAccess::command(
        editor, window, MAKEWPARAM(IDC_AUTOMATION_VIEW, CBN_SELCHANGE));
    MINMAXINFO limits{};
    SendMessageW(window, WM_GETMINMAXINFO, 0,
                 reinterpret_cast<LPARAM>(&limits));
    resize(limits.ptMinTrackSize.x, limits.ptMinTrackSize.y);
    RECT client{};
    GetClientRect(window, &client);
    const SIZE minimum = AutomationEditorTestAccess::minimum_client(editor);
    expect(minimum.cx > 0 && client.right >= minimum.cx &&
               client.bottom >= minimum.cy,
           "the " + name + " view's minimum frame holds its measured extent");
    std::vector<std::pair<int, RECT>> shown;
    for (HWND child = GetWindow(window, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT))
      if (visible(child))
        shown.emplace_back(GetDlgCtrlID(child), client_rect(child));
    for (std::size_t i = 0; i < shown.size(); ++i) {
      const RECT &rect = shown[i].second;
      expect(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right &&
                 rect.bottom <= client.bottom,
             "control " + std::to_string(shown[i].first) + " of the " + name +
                 " view fits the minimum client area");
      for (std::size_t j = i + 1; j < shown.size(); ++j) {
        RECT overlap{};
        expect(IntersectRect(&overlap, &rect, &shown[j].second) == FALSE,
               "controls " + std::to_string(shown[i].first) + " and " +
                   std::to_string(shown[j].first) + " of the " + name +
                   " view do not overlap at the minimum size");
      }
    }
    resize(200, 150);
    GetClientRect(window, &client);
    expect(client.right >= minimum.cx && client.bottom >= minimum.cy,
           "a " + name + " window made too small grows back to its extent");
  }

  const HWND speed = GetDlgItem(window, IDC_AUTOMATION_SIM_SPEED);
  const HWND speed_label = GetDlgItem(window, IDC_AUTOMATION_SIM_SPEED_LABEL);
  expect(speed != nullptr && speed_label != nullptr &&
             GetWindow(speed, GW_HWNDPREV) == speed_label &&
             window_text(speed_label) == L"Playbac&k speed",
         "the speed list follows its visible label, which names it");
  int access_k = 0;
  for (HWND child = GetWindow(window, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    const std::wstring text = window_text(child);
    if (text.find(L"&k") != std::wstring::npos ||
        text.find(L"&K") != std::wstring::npos)
      ++access_k;
  }
  expect(access_k == 1, "Alt+K reaches only the speed list");
  for (int view = 0; view < static_cast<int>(view_names.size()); ++view) {
    SendDlgItemMessageW(window, IDC_AUTOMATION_VIEW, CB_SETCURSEL,
                        static_cast<WPARAM>(view), 0);
    (void)AutomationEditorTestAccess::command(
        editor, window, MAKEWPARAM(IDC_AUTOMATION_VIEW, CBN_SELCHANGE));
    expect(visible(speed_label) == (view == 1) &&
               visible(speed) == (view == 1),
           "the speed label shows and hides with the Simulator view");
  }
}

// Up/Down, Shift and the wheel step through the binding's one numeric domain,
// so stepping can never manufacture a value typing would be refused (review
// GUI-01: Width 1200 + Up used to give 1201).
void editor_profile_field_stepping_contracts() {
  using layerc::editor::stepped_field_text;
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10,
                                    nullptr, nullptr, instance, nullptr);
  if (!expect(host != nullptr, "the stepping host window should open"))
    return;
  const auto profile = layerc::editor::make_new_profile();
  const layerc::core::CooldownDefinition cooldown{};
  const layerc::core::Panel panel{};
  const layerc::editor::ProfileFieldView view{host, profile, &cooldown, &panel};
  int stepped = 0;
  for (const auto &binding : layerc::editor::profile_field_bindings()) {
    if (binding.stepping == nullptr)
      continue;
    ++stepped;
    const HWND edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD, 0, 0, 10, 10, host,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(binding.control_id)),
        instance, nullptr);
    const auto accepts = [&](const std::wstring &text) {
      SetWindowTextW(edit, text.c_str());
      return binding.valid(view);
    };
    const auto &range = *binding.stepping;
    const std::wstring top = stepped_field_text(L"1e12", range, 0);
    const std::wstring bottom = stepped_field_text(L"-1e12", range, 0);
    const std::wstring name = std::to_wstring(binding.control_id);
    expect(accepts(top) && accepts(bottom),
           "both stepping bounds of field " +
               layerc::platform::wide_to_utf8(name) + " are valid");
    for (const int steps : {1, 10, -1, -10}) {
      expect(accepts(stepped_field_text(top, range, steps)) &&
                 accepts(stepped_field_text(bottom, range, steps)) &&
                 accepts(stepped_field_text(L"not a number", range, steps)),
             "stepping field " + layerc::platform::wide_to_utf8(name) +
                 " stays inside its validated domain");
    }
    expect(stepped_field_text(top, range, 10) == top &&
               stepped_field_text(bottom, range, -10) == bottom,
           "stepping clamps at both bounds of field " +
               layerc::platform::wide_to_utf8(name));
    DestroyWindow(edit);
  }
  DestroyWindow(host);
  expect(stepped == 4, "duration, charges, width and height step");

  const auto *width = layerc::editor::profile_field_binding(IDC_BAR_WIDTH);
  const auto *height = layerc::editor::profile_field_binding(IDC_BAR_HEIGHT);
  const auto *duration =
      layerc::editor::profile_field_binding(IDC_COOLDOWN_DURATION);
  if (!expect(width != nullptr && width->stepping != nullptr &&
                  height != nullptr && height->stepping != nullptr &&
                  duration != nullptr && duration->stepping != nullptr,
              "width, height and duration expose their stepping"))
    return;
  expect(stepped_field_text(L"1200", *width->stepping, 1) == L"1200" &&
             stepped_field_text(L"1199", *width->stepping, 10) == L"1200" &&
             stepped_field_text(L"40", *width->stepping, -1) == L"40" &&
             stepped_field_text(L"5000", *width->stepping, -1) == L"1199",
         "width steps inside 40-1200");
  expect(stepped_field_text(L"240", *height->stepping, 1) == L"240" &&
             stepped_field_text(L"16", *height->stepping, -10) == L"16",
         "height steps inside 16-240");
  expect(stepped_field_text(L"1.25", *duration->stepping, 1) == L"1.4" &&
             stepped_field_text(L"0.1", *duration->stepping, -1) == L"0.1" &&
             stepped_field_text(L"86400", *duration->stepping, 1) ==
                 L"86400.0",
         "duration steps in tenths from 0.1 s to 86400 s");
}
