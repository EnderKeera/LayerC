#include "cooldown_editor.hpp"
#include "editor_util.hpp"
#include "profile_document.hpp"
#include "test_support.hpp"
#include "visual_editor.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The cooldown and visual sub-editors are components: each is driven here
// through its injected document, shell, and host callbacks alone, without
// an EditorWindow.
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

const InputBinding *binding_for(const GameProfile &profile,
                                const std::string &cooldown_id) {
  for (const auto &binding : profile.bindings)
    if (binding.cooldown_id == cooldown_id) return &binding;
  return nullptr;
}

} // namespace

void editor_cooldown_component_contracts() {
  ProfileDocument document;
  document.profile() = layerc::editor::make_new_profile();
  layerc::editor::EditorShell shell;
  const NullTheme theme;
  HostCalls calls;
  CooldownEditor editor(
      document, shell, theme,
      [&](const DocumentChangeKind kind) {
        if (kind == DocumentChangeKind::immediate) ++calls.immediate_changes;
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
  const auto &added = document.profile().cooldowns.back();
  expect(document.profile().cooldowns.size() == before + 1U &&
             editor.selected() == &added,
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

  SendMessageW(list.get(), LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
  editor.begin_capture();
  expect(editor.selected() == nullptr && !editor.capturing_binding(),
         "capture needs a selected cooldown");
  expect(!editor.handle_capture_message(key_down('Q', 0x10)),
         "keys pass through when capture is idle");
}

void editor_visual_component_contracts() {
  ProfileDocument document;
  document.profile() = layerc::editor::make_new_profile();
  document.path() = L"C:\\fixture\\profile.json";
  const auto cooldown_id = document.profile().cooldowns.front().id;
  document.visual().emplace();
  document.visual()->profile_id = document.profile().id;
  document.visual()->observations.push_back(
      PixelRule{.id = "ready", .name = "Ready"});
  document.visual()->triggers.push_back(
      {.cooldown_id = cooldown_id, .observation_id = "ready"});
  document.mark_visual_edited();
  document.history().reset(document.profile(), document.visual(),
                           document.automation());

  layerc::editor::EditorShell shell;
  const layerc::editor::LiveEditSession live_edit;
  layerc::editor::TaskPicker task_picker;
  auto commit = layerc::editor::CommitResult::ok;
  bool replace_modules = false;
  int commits{};
  int committed{};
  int tasks_shown{};
  VisualEditor editor(
      document, shell, live_edit, task_picker,
      {.selected_cooldown =
           [&]() -> const CooldownDefinition * {
             return &document.profile().cooldowns.front();
           },
       .commit_document =
           [&] {
             ++commits;
             // The real pipeline replaces the document's modules even when
             // the commit is refused.
             if (replace_modules) {
               auto replacement = document.visual();
               document.visual() = std::move(replacement);
             }
             return commit;
           },
       .mark_committed = [&] { ++committed; },
       .write_failure_message = [] { return std::wstring(L"fixture"); },
       .require_stored_profile = [](const wchar_t *) { return true; },
       .show_tasks = [&] { ++tasks_shown; }});

  editor.set_enabled(false);
  expect(!document.visual()->triggers.front().enabled && commits == 1 &&
             committed == 1,
         "disabling commits through the host's pipeline");

  commit = layerc::editor::CommitResult::upgrade_declined;
  editor.set_enabled(true);
  expect(!document.visual()->triggers.front().enabled && commits == 2 &&
             committed == 1,
         "a declined commit restores the previous enablement");

  replace_modules = true;
  editor.set_enabled(true);
  replace_modules = false;
  expect(!document.visual()->triggers.front().enabled && commits == 3 &&
             committed == 1,
         "a refused toggle restores enablement in the replaced document");

  editor.calibrate();
  expect(task_picker.awaiting_calibration() && tasks_shown == 1,
         "without a live target, calibration asks the host for a task");
}
