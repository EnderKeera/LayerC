#include "visual_editor.hpp"

#include "editor_prompt.hpp"
#include "editor_resource.h"
#include "editor_text.hpp"
#include "editor_util.hpp"
#include "calibration_session.hpp"
#include "visual_assignment.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <windows.h>

namespace layerc::editor {
namespace {

std::wstring visual_mode_label(const layerc::core::VisualTrigger &trigger) {
  return trigger.mode == layerc::core::VisualTriggerMode::pixel_only
             ? L"Pixel only"
             : L"Confirm input";
}

std::optional<layerc::platform::WindowCandidate> candidate_for_window(
    const HWND window) {
  for (auto candidate : layerc::platform::enumerate_attachable_windows()) {
    if (candidate.window == window) return candidate;
  }
  return std::nullopt;
}

} // namespace

VisualEditor::VisualEditor(ProfileDocument &document, EditorShell &shell,
                           const LiveEditSession &live_edit,
                           TaskPicker &task_picker, Host host) noexcept
    : document_(document), shell_(shell), live_edit_(live_edit),
      task_picker_(task_picker), host_(std::move(host)) {}

void VisualEditor::set_status(const std::wstring_view text) const {
  set_text(shell_.status, text);
}

void VisualEditor::refresh() {
  if (shell_.window == nullptr) return;
  const HWND summary = GetDlgItem(shell_.window, IDC_VISUAL_SUMMARY);
  const HWND enabled = GetDlgItem(shell_.window, IDC_VISUAL_ENABLED);
  const HWND calibrate = GetDlgItem(shell_.window, IDC_CALIBRATE_VISUAL);
  const HWND remove = GetDlgItem(shell_.window, IDC_REMOVE_VISUAL);
  const HWND isolated = GetDlgItem(shell_.window, IDC_VISUAL_ISOLATED);
  set_text(GetDlgItem(shell_.window, IDC_VISUAL_RUNTIME_STATUS),
           shell_.visual_runtime_status);

  const auto *cooldown = host_.selected_cooldown();
  const bool editable = cooldown != nullptr && !document_.profile().game.global;
  const layerc::core::VisualTrigger *trigger{};
  const layerc::core::PixelRule *rule{};
  const layerc::core::TemplateState *template_state{};
  if (editable && document_.visual()) {
    trigger = find_visual_trigger(*document_.visual(), cooldown->id);
    if (trigger != nullptr) {
      const auto *observation = layerc::core::find_observation(
          *document_.visual(), trigger->observation_id);
      rule = std::get_if<layerc::core::PixelRule>(observation);
      template_state = std::get_if<layerc::core::TemplateState>(observation);
    }
  }

  // The group edits the selected cooldown's trigger; say which one.
  set_text(GetDlgItem(shell_.window, IDC_VISUAL_SECTION_LABEL),
           cooldown != nullptr ? L"Visual trigger for " + to_wide(cooldown->name)
                               : std::wstring(L"Visual trigger"));
  std::wstring text = L"No visual trigger";
  if (document_.profile().game.global) {
    text = L"Visual capture is unavailable for global profiles";
  } else if (document_.visual_state() ==
             layerc::platform::ModuleSidecarState::invalid) {
    text = L"Invalid visual settings — all triggers blocked";
    if (!document_.visual_error().empty())
      text += L": " + to_wide(document_.visual_error());
  } else if (trigger != nullptr) {
    text = visual_mode_label(*trigger);
    if (rule != nullptr && !rule->name.empty())
      text += L" — Pixel: " + to_wide(rule->name);
    else if (template_state != nullptr && !template_state->name.empty())
      text += L" — Template: " + to_wide(template_state->name);
    if (document_.visual_state() ==
        layerc::platform::ModuleSidecarState::recovered_backup)
      text += L" (recovered backup)";
  }
  set_text(summary, text);
  set_owner_draw_checkbox(enabled, trigger != nullptr && trigger->enabled);
  set_owner_draw_checkbox(
      isolated,
      document_.profile().visual_runtime == layerc::core::VisualRuntime::isolated);
  EnableWindow(enabled, trigger != nullptr);
  EnableWindow(remove, trigger != nullptr);
  EnableWindow(isolated, !document_.profile().game.global);
  // Calibration can repair an invalid sidecar by replacing the selected
  // assignment with a newly validated single-observation result.
  EnableWindow(calibrate, editable && !document_.path().empty());
  // Every unavailable control in the group gets its reason inline.
  const wchar_t *hint = L"";
  if (cooldown == nullptr)
    hint = L"Select a cooldown to give it a visual trigger.";
  else if (document_.path().empty())
    hint = L"Save the profile once to calibrate a visual trigger.";
  else if (trigger == nullptr && !document_.profile().game.global)
    hint = L"Calibrate captures what the game shows when this cooldown "
           L"starts.";
  set_text(GetDlgItem(shell_.window, IDC_VISUAL_HINT), hint);
  InvalidateRect(enabled, nullptr, TRUE);
  InvalidateRect(isolated, nullptr, TRUE);
}

bool VisualEditor::save() {
  if (!document_.visual() || document_.path().empty()) return false;
  switch (host_.commit_document()) {
  case CommitResult::ok:
    break;
  case CommitResult::invalid_fields:
  case CommitResult::invalid_profile:
    themed_message_box(shell_.window,
                L"Finish the invalid profile field before committing visual "
                L"recognition.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  case CommitResult::upgrade_declined:
    return false;
  case CommitResult::blocked_by_sidecar:
    themed_message_box(shell_.window,
                L"Repair the invalid automation settings before saving "
                L"visual recognition. This prevents an unrelated visual edit "
                L"from silently discarding recovery data.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  case CommitResult::write_failed:
    themed_message_box(shell_.window, host_.write_failure_message().c_str(),
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  document_.mark_visual_edited();
  (void)document_.history().record(document_.profile(), document_.visual(), document_.automation(), false);
  host_.mark_committed();
  // Persisted sidecars are not the whole retention set: undo/redo and other
  // interactive calibration sessions can still own content-addressed assets.
  // Do not run offline asset maintenance from an interactive commit.
  refresh();
  return true;
}

void VisualEditor::set_enabled(const bool enabled) {
  const auto *cooldown = host_.selected_cooldown();
  if (cooldown == nullptr || !document_.visual()) {
    refresh();
    return;
  }
  // A commit replaces the document's modules even when it is refused, so
  // neither the trigger nor the cooldown may be held across save().
  const std::string cooldown_id = cooldown->id;
  auto *trigger = find_visual_trigger(*document_.visual(), cooldown_id);
  if (trigger == nullptr) {
    refresh();
    return;
  }
  const bool previous = trigger->enabled;
  trigger->enabled = enabled;
  if (!save()) {
    if (document_.visual())
      if (auto *current = find_visual_trigger(*document_.visual(), cooldown_id))
        current->enabled = previous;
    refresh();
    return;
  }
  set_status(enabled ? L"Visual assignment enabled."
                     : L"Visual assignment disabled; ordinary input restored.");
}

void VisualEditor::remove_assignment() {
  const auto *cooldown = host_.selected_cooldown();
  if (cooldown == nullptr || !document_.visual()) return;
  const auto *existing = find_visual_trigger(*document_.visual(), cooldown->id);
  if (existing == nullptr) return;
  if (themed_message_box(shell_.window,
                  L"Remove visual recognition from this cooldown? Its ordinary input binding is preserved.",
                  L"Remove visual assignment", MB_OKCANCEL | MB_ICONQUESTION) !=
      IDOK)
    return;

  const auto previous_visual = document_.visual();
  erase_visual_assignment(*document_.visual(), cooldown->id);
  if (save())
    set_status(L"Visual assignment removed; ordinary input remains active.");
  else {
    document_.visual() = previous_visual;
    refresh();
  }
}

void VisualEditor::calibrate() {
  const auto *cooldown = host_.selected_cooldown();
  if (cooldown == nullptr) return;
  if (document_.profile().game.global) {
    themed_message_box(shell_.window,
                L"Visual recognition needs a specific matched game window and is unavailable for global profiles.",
                L"Visual calibration", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!host_.require_stored_profile(L"Save this new profile before calibrating it."))
    return;

  auto matching = document_.profile();
  matching.enabled = true;
  if (live_edit_.target() != nullptr && IsWindow(live_edit_.target())) {
    const auto candidate = candidate_for_window(live_edit_.target());
    if (candidate && layerc::platform::profile_matches_window(*candidate,
                                                              matching)) {
      launch_calibration(live_edit_.target());
      return;
    }
  }
  task_picker_.request_calibration();
  host_.show_tasks();
  set_status(L"Select a matching game window, then choose Calibrate Visual.");
}

void VisualEditor::calibrate_task(
    const layerc::platform::WindowCandidate &candidate) {
  auto matching = document_.profile();
  matching.enabled = true;
  if (!layerc::platform::profile_matches_window(candidate, matching)) {
    themed_message_box(task_picker_.window,
                L"That window does not match this profile. Use Selected Task first if you intend to change the matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (task_picker_.awaiting_calibration())
    task_picker_.clear_request();
  ShowWindow(task_picker_.window, SW_HIDE);
  if (!launch_calibration(candidate.window)) {
    ShowWindow(task_picker_.window, SW_SHOWNORMAL);
    SetForegroundWindow(task_picker_.window);
    set_status(L"Visual calibration was not started.");
  }
}

bool VisualEditor::launch_calibration(const HWND target) {
  const auto *cooldown = host_.selected_cooldown();
  if (cooldown == nullptr || target == nullptr || !IsWindow(target))
    return false;
  const auto notice = layerc::platform::read_bool_setting(
          layerc::platform::BoolSetting::visual_capture_notice);
  if (notice.failed()) {
    themed_message_box(shell_.window, L"Could not read the visual capture acknowledgement.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!notice.value.value_or(false)) {
    if (themed_message_box(
            shell_.window,
            L"Visual recognition captures pixels from the selected game window without injecting into or hooking the game. Protected content can appear black, and anti-cheat compatibility cannot be guaranteed. Continue?",
            L"Visual capture notice", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
      return false;
    if (!layerc::platform::write_bool_setting(
          layerc::platform::BoolSetting::visual_capture_notice, true)) {
      themed_message_box(shell_.window, L"Could not save the visual capture acknowledgement.",
                  L"LayerC Editor", MB_OK | MB_ICONWARNING);
      return false;
    }
  }

  layerc::core::CalibrationRequest request{
      .target_window = reinterpret_cast<std::uintptr_t>(target),
      .cooldown_id = cooldown->id,
      .profile_json = layerc::core::serialize_profile_json(document_.profile()),
      .visual_json = document_.visual()
                         ? layerc::core::serialize_visual_profile_json(
                               *document_.visual())
                         : std::string{},
  };
  auto session = run_calibration_session(shell_.window, request);
  if (const auto *failure = std::get_if<CalibrationFailure>(&session)) {
    show_calibration_failure(shell_.window, *failure);
    // Quit is handled by the outer loop, not by reopening the task picker.
    return failure->kind == CalibrationFailureKind::cancelled;
  }
  const auto &result = std::get<layerc::core::CalibrationResponse>(session);
  if (result.applied) {
    if (!merge_calibration_result(result.visual_json)) return false;
    set_status(L"Visual calibration applied and committed.");
  } else {
    set_status(L"Visual calibration cancelled; no changes were made.");
  }
  return true;
}

bool VisualEditor::merge_calibration_result(const std::string_view visual_json) {
  const auto *cooldown = host_.selected_cooldown();
  auto merged = layerc::editor::merge_calibration_result(
      document_.visual(),
      document_.visual_state() == layerc::platform::ModuleSidecarState::invalid,
      document_.profile(),
      cooldown == nullptr ? std::string_view{} : std::string_view{cooldown->id},
      visual_json, [&] {
        const int choice = themed_message_box(
            shell_.window,
            L"This visual observation is shared by multiple cooldowns. Choose Yes to update the shared observation, No to clone it for this cooldown, or Cancel to keep everything unchanged.",
            L"Shared visual observation", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return SharedObservationChoice::cancel;
        return choice == IDYES ? SharedObservationChoice::update_shared
                               : SharedObservationChoice::clone;
      });
  if (const auto *refusal = std::get_if<VisualMergeRefusal>(&merged)) {
    if (refusal->kind == VisualMergeRefusal::Kind::limit)
      themed_message_box(shell_.window, to_wide(refusal->message).c_str(),
                         L"Visual observation limit", MB_OK | MB_ICONINFORMATION);
    else if (refusal->kind != VisualMergeRefusal::Kind::cancelled)
      themed_message_box(shell_.window, to_wide(refusal->message).c_str(),
                         L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto previous = document_.visual();
  document_.visual() = std::move(std::get<layerc::core::VisualProfile>(merged));
  if (!save()) {
    document_.visual() = previous;
    refresh();
    return false;
  }
  return true;
}

} // namespace layerc::editor
