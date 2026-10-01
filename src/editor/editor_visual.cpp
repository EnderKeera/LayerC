#include "editor_window.hpp"
#include "editor_resource.h"
#include "calibration_session.hpp"

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

layerc::core::VisualTrigger *find_trigger(
    layerc::core::VisualProfile &visual, const std::string_view cooldown) {
  const auto found = std::ranges::find(visual.triggers, cooldown,
                                      &layerc::core::VisualTrigger::cooldown_id);
  return found == visual.triggers.end() ? nullptr : &*found;
}

using VisualObservation =
    std::variant<layerc::core::PixelRule, layerc::core::TemplateState>;

std::string observation_id(const VisualObservation &observation) {
  return std::visit([](const auto &item) { return item.id; }, observation);
}

void set_observation_id(VisualObservation &observation,
                        const std::string_view id) {
  std::visit([&](auto &item) { item.id = id; }, observation);
}

std::string unique_observation_id(
    const std::string_view requested,
    const layerc::core::VisualProfile &profile,
    const std::string_view allowed = {}) {
  const std::string base =
      requested.empty() ? "visual-observation" : std::string(requested);
  return next_unique_id(
      base, [&](const std::string_view value) {
        if (!allowed.empty() && value == allowed) return false;
        return layerc::core::has_observation(profile, value);
      });
}

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

bool ensure_observation_capacity(
    const HWND owner, const layerc::core::VisualProfile &profile,
    const VisualObservation &candidate,
    const std::string_view replacing_id = {}) {
  const bool is_pixel = std::holds_alternative<layerc::core::PixelRule>(candidate);
  std::size_t count = is_pixel ? profile.rules.size() : profile.templates.size();
  if (!replacing_id.empty()) {
    const bool replaces_same_type =
        is_pixel ? layerc::core::find_pixel_rule(profile, replacing_id) != nullptr
                 : layerc::core::find_template_state(profile, replacing_id) != nullptr;
    if (replaces_same_type) --count;
  }
  const auto maximum = is_pixel ? layerc::core::max_pixel_rules
                                : layerc::core::max_template_states;
  if (count < maximum) return true;
  const auto kind = is_pixel ? L"pixel" : L"template";
  const auto message = L"This profile already has the maximum " +
                       std::to_wstring(maximum) + L" " + kind +
                       L" observations.";
  themed_message_box(owner, message.c_str(), L"Visual observation limit",
              MB_OK | MB_ICONINFORMATION);
  return false;
}

void append_observation(layerc::core::VisualProfile &profile,
                        const VisualObservation &observation) {
  std::visit([&](const auto &item) {
    using T = std::decay_t<decltype(item)>;
    if constexpr (std::is_same_v<T, layerc::core::PixelRule>)
      profile.rules.push_back(item);
    else
      profile.templates.push_back(item);
  }, observation);
}

void replace_observation(layerc::core::VisualProfile &profile,
                         const std::string_view old_id,
                         const VisualObservation &observation) {
  std::erase_if(profile.rules, [&](const auto &item) { return item.id == old_id; });
  std::erase_if(profile.templates,
                [&](const auto &item) { return item.id == old_id; });
  append_observation(profile, observation);
}

// Fresh observation id, append, and trigger link for a calibration result
// that has no shared prior observation to reuse.
bool install_new_observation(const HWND owner,
                             layerc::core::VisualProfile &profile,
                             VisualObservation &observation,
                             layerc::core::VisualTrigger &trigger) {
  if (!ensure_observation_capacity(owner, profile, observation))
    return false;
  const auto new_id =
      unique_observation_id(observation_id(observation), profile);
  set_observation_id(observation, new_id);
  trigger.observation_id = new_id;
  append_observation(profile, observation);
  return true;
}

std::optional<VisualObservation>
single_observation(const layerc::core::VisualProfile &profile) {
  if (profile.rules.size() + profile.templates.size() != 1) return std::nullopt;
  if (!profile.rules.empty()) return VisualObservation{profile.rules.front()};
  return VisualObservation{profile.templates.front()};
}

} // namespace

void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             const std::string_view cooldown_id) {
  std::string old_rule;
  if (const auto *trigger = find_trigger(visual, cooldown_id))
    old_rule = trigger->observation_id;
  std::erase_if(visual.triggers, [&](const auto &t) {
    return t.cooldown_id == cooldown_id;
  });
  gc_orphan_visual_observation(visual, old_rule);
}

void gc_orphan_visual_observation(layerc::core::VisualProfile &visual,
                                  const std::string_view observation_id) {
  if (observation_id.empty()) return;
  if (std::ranges::any_of(visual.triggers, [&](const auto &t) {
        return t.observation_id == observation_id;
      }))
    return;
  std::erase_if(visual.rules,
                [&](const auto &r) { return r.id == observation_id; });
  std::erase_if(visual.templates,
                [&](const auto &state) { return state.id == observation_id; });
}

void EditorWindow::refresh_visual_controls() {
  if (shell_.window == nullptr) return;
  const HWND summary = GetDlgItem(shell_.window, IDC_VISUAL_SUMMARY);
  const HWND enabled = GetDlgItem(shell_.window, IDC_VISUAL_ENABLED);
  const HWND calibrate = GetDlgItem(shell_.window, IDC_CALIBRATE_VISUAL);
  const HWND remove = GetDlgItem(shell_.window, IDC_REMOVE_VISUAL);
  const HWND isolated = GetDlgItem(shell_.window, IDC_VISUAL_ISOLATED);
  set_text(GetDlgItem(shell_.window, IDC_VISUAL_RUNTIME_STATUS),
           shell_.visual_runtime_status);

  const auto *cooldown = selected_cooldown();
  const bool editable = cooldown != nullptr && !document_.profile().game.global;
  const layerc::core::VisualTrigger *trigger{};
  const layerc::core::PixelRule *rule{};
  const layerc::core::TemplateState *template_state{};
  if (editable && document_.visual()) {
    trigger = find_trigger(*document_.visual(), cooldown->id);
    if (trigger != nullptr) {
      rule = layerc::core::find_pixel_rule(*document_.visual(),
                                           trigger->observation_id);
      template_state = layerc::core::find_template_state(
          *document_.visual(), trigger->observation_id);
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

bool EditorWindow::save_visual_profile() {
  if (!document_.visual() || document_.path().empty()) return false;
  switch (commit_bundle(document_.path())) {
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
    themed_message_box(shell_.window, write_failure_message().c_str(),
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  document_.mark_visual_edited();
  (void)document_.history().record(document_.profile(), document_.visual(), document_.automation(), false);
  mark_committed();
  // Persisted sidecars are not the whole retention set: undo/redo and other
  // interactive calibration sessions can still own content-addressed assets.
  // Do not run offline asset maintenance from an interactive commit.
  refresh_visual_controls();
  return true;
}

void EditorWindow::set_visual_assignment_enabled(const bool enabled) {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || !document_.visual()) {
    refresh_visual_controls();
    return;
  }
  auto *trigger = find_trigger(*document_.visual(), cooldown->id);
  if (trigger == nullptr) {
    refresh_visual_controls();
    return;
  }
  const bool previous = trigger->enabled;
  trigger->enabled = enabled;
  if (!save_visual_profile()) {
    trigger->enabled = previous;
    refresh_visual_controls();
    return;
  }
  set_status(enabled ? L"Visual assignment enabled."
                     : L"Visual assignment disabled; ordinary input restored.");
}

void EditorWindow::remove_visual_assignment() {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || !document_.visual()) return;
  const auto *existing = find_trigger(*document_.visual(), cooldown->id);
  if (existing == nullptr) return;
  if (themed_message_box(shell_.window,
                  L"Remove visual recognition from this cooldown? Its ordinary input binding is preserved.",
                  L"Remove visual assignment", MB_OKCANCEL | MB_ICONQUESTION) !=
      IDOK)
    return;

  const auto previous_visual = document_.visual();
  erase_visual_assignment(*document_.visual(), cooldown->id);
  if (save_visual_profile())
    set_status(L"Visual assignment removed; ordinary input remains active.");
  else {
    document_.visual() = previous_visual;
    refresh_visual_controls();
  }
}

void EditorWindow::calibrate_visual() {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) return;
  if (document_.profile().game.global) {
    themed_message_box(shell_.window,
                L"Visual recognition needs a specific matched game window and is unavailable for global profiles.",
                L"Visual calibration", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!require_stored_profile(L"Save this new profile before calibrating it."))
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
  show_tasks();
  set_status(L"Select a matching game window, then choose Calibrate Visual.");
}

void EditorWindow::calibrate_selected_task() {
  const auto *candidate = require_selected_task();
  if (candidate == nullptr) return;
  auto matching = document_.profile();
  matching.enabled = true;
  if (!layerc::platform::profile_matches_window(*candidate, matching)) {
    themed_message_box(task_picker_.window,
                L"That window does not match this profile. Use Selected Task first if you intend to change the matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (task_picker_.awaiting_calibration())
    task_picker_.clear_request();
  ShowWindow(task_picker_.window, SW_HIDE);
  if (!launch_calibration(candidate->window)) {
    ShowWindow(task_picker_.window, SW_SHOWNORMAL);
    SetForegroundWindow(task_picker_.window);
    set_status(L"Visual calibration was not started.");
  }
}

bool EditorWindow::launch_calibration(const HWND target) {
  const auto *cooldown = selected_cooldown();
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

bool EditorWindow::merge_calibration_result(const std::string_view visual_json) {
  const auto parsed =
      layerc::core::parse_visual_profile_json(visual_json, document_.profile());
  const auto *candidate_profile =
      std::get_if<layerc::core::VisualProfile>(&parsed);
  const auto *cooldown = selected_cooldown();
  const auto candidate = candidate_profile == nullptr
                             ? std::optional<VisualObservation>{}
                             : single_observation(*candidate_profile);
  if (candidate_profile == nullptr || cooldown == nullptr || !candidate ||
      candidate_profile->triggers.size() != 1 ||
      candidate_profile->triggers.front().cooldown_id != cooldown->id) {
    themed_message_box(shell_.window,
                L"LayerC Calibration returned an invalid result. No profile "
                L"data was changed.",
                L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }

  layerc::core::VisualProfile merged =
      document_.visual().value_or(layerc::core::VisualProfile{});
  if (document_.visual_state() == layerc::platform::ModuleSidecarState::invalid)
    merged = {};
  merged.profile_id = document_.profile().id;
  merged.sample_rate_hz = candidate_profile->sample_rate_hz;
  merged.quality = candidate_profile->quality;
  auto candidate_observation = *candidate;
  auto candidate_trigger = candidate_profile->triggers.front();
  auto *old_trigger = find_trigger(merged, cooldown->id);
  std::string old_observation_id;
  if (old_trigger != nullptr) {
    old_observation_id = old_trigger->observation_id;
    const auto references = std::ranges::count_if(
        merged.triggers,
        [&](const auto &trigger) {
          return trigger.observation_id == old_observation_id;
        });
    bool update_shared = references <= 1;
    if (references > 1) {
      const int choice = themed_message_box(
          shell_.window,
          L"This visual observation is shared by multiple cooldowns. Choose Yes to update the shared observation, No to clone it for this cooldown, or Cancel to keep everything unchanged.",
          L"Shared visual observation", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (choice == IDCANCEL) return false;
      update_shared = choice == IDYES;
    }
    if (update_shared) {
      if (!ensure_observation_capacity(shell_.window, merged,
                                       candidate_observation,
                                       old_observation_id))
        return false;
      set_observation_id(candidate_observation, old_observation_id);
      replace_observation(merged, old_observation_id, candidate_observation);
      candidate_trigger.observation_id = old_observation_id;
    } else if (!install_new_observation(shell_.window, merged,
                                        candidate_observation,
                                        candidate_trigger)) {
      return false;
    }
    *old_trigger = candidate_trigger;
  } else {
    if (!install_new_observation(shell_.window, merged, candidate_observation,
                                 candidate_trigger))
      return false;
    merged.triggers.push_back(candidate_trigger);
  }

  gc_orphan_visual_observation(merged, old_observation_id);

  const auto error = layerc::core::validate_visual_profile(merged, document_.profile());
  if (!error.message.empty()) {
    themed_message_box(shell_.window, to_wide(error.message).c_str(),
                L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto previous = document_.visual();
  document_.visual() = std::move(merged);
  if (!save_visual_profile()) {
    document_.visual() = previous;
    refresh_visual_controls();
    return false;
  }
  return true;
}

} // namespace layerc::editor
