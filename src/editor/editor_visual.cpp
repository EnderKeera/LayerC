#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/framed_pipe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
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

const layerc::core::PixelRule *find_rule(
    const layerc::core::VisualProfile &visual, const std::string_view id) {
  const auto found =
      std::ranges::find(visual.rules, id, &layerc::core::PixelRule::id);
  return found == visual.rules.end() ? nullptr : &*found;
}

std::string unique_rule_id(const std::string_view requested,
                           const std::vector<layerc::core::PixelRule> &rules,
                           const std::string_view allowed = {}) {
  const std::string base = requested.empty() ? "visual-rule" : std::string(requested);
  auto used = [&](const std::string_view value) {
    if (!allowed.empty() && value == allowed) return false;
    return std::ranges::any_of(rules, [&](const auto &rule) {
      return rule.id == value;
    });
  };
  std::string candidate = base;
  for (std::size_t suffix = 2; used(candidate); ++suffix)
    candidate = base + "-" + std::to_string(suffix);
  return candidate;
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

void close_handle(HANDLE &handle) {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
}

bool ensure_visual_rule_capacity(
    const HWND owner, const layerc::core::VisualProfile &profile) {
  if (profile.rules.size() < layerc::core::max_pixel_rules) return true;
  const auto message =
      L"This profile already has the maximum " +
      std::to_wstring(layerc::core::max_pixel_rules) + L" visual rules.";
  MessageBoxW(owner, message.c_str(), L"Visual rule limit",
              MB_OK | MB_ICONINFORMATION);
  return false;
}

} // namespace

void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             const std::string_view cooldown_id) {
  std::string old_rule;
  for (const auto &trigger : visual.triggers) {
    if (trigger.cooldown_id == cooldown_id) {
      old_rule = trigger.rule_id;
      break;
    }
  }
  std::erase_if(visual.triggers, [&](const auto &t) {
    return t.cooldown_id == cooldown_id;
  });
  gc_orphan_visual_rule(visual, old_rule);
}

void gc_orphan_visual_rule(layerc::core::VisualProfile &visual,
                           const std::string_view rule_id) {
  if (rule_id.empty()) return;
  if (std::none_of(visual.triggers.begin(), visual.triggers.end(),
                   [&](const auto &t) { return t.rule_id == rule_id; })) {
    std::erase_if(visual.rules,
                  [&](const auto &r) { return r.id == rule_id; });
  }
}

void EditorWindow::load_current_visual_profile() {
  visual_profile_.reset();
  visual_sidecar_state_ = layerc::platform::VisualSidecarState::absent;
  visual_sidecar_error_.clear();
  if (!current_path_.empty()) {
    auto loaded =
        layerc::platform::load_visual_sidecar(current_path_, profile_);
    visual_sidecar_state_ = loaded.state;
    visual_profile_ = std::move(loaded.profile);
    visual_sidecar_error_ = std::move(loaded.error.message);
  }
  refresh_visual_controls();
}

void EditorWindow::refresh_visual_controls() {
  if (dialog_ == nullptr) return;
  const HWND summary = GetDlgItem(dialog_, IDC_VISUAL_SUMMARY);
  const HWND enabled = GetDlgItem(dialog_, IDC_VISUAL_ENABLED);
  const HWND calibrate = GetDlgItem(dialog_, IDC_CALIBRATE_VISUAL);
  const HWND remove = GetDlgItem(dialog_, IDC_REMOVE_VISUAL);
  set_text(GetDlgItem(dialog_, IDC_VISUAL_RUNTIME_STATUS),
           visual_runtime_status_);

  const auto *cooldown = selected_cooldown();
  const bool editable = cooldown != nullptr && !profile_.game.global;
  const layerc::core::VisualTrigger *trigger{};
  const layerc::core::PixelRule *rule{};
  if (editable && visual_profile_) {
    trigger = find_trigger(*visual_profile_, cooldown->id);
    if (trigger != nullptr) rule = find_rule(*visual_profile_, trigger->rule_id);
  }

  std::wstring text = L"Input only";
  if (profile_.game.global) {
    text = L"Visual capture is unavailable for global profiles";
  } else if (visual_sidecar_state_ ==
             layerc::platform::VisualSidecarState::invalid) {
    text = L"Invalid visual sidecar — all triggers blocked";
    if (!visual_sidecar_error_.empty())
      text += L": " + to_wide(visual_sidecar_error_);
  } else if (trigger != nullptr) {
    text = visual_mode_label(*trigger);
    if (rule != nullptr && !rule->name.empty())
      text += L" — " + to_wide(rule->name);
    if (visual_sidecar_state_ ==
        layerc::platform::VisualSidecarState::recovered_backup)
      text += L" (recovered backup)";
  }
  set_text(summary, text);
  set_owner_draw_checkbox(enabled, trigger != nullptr && trigger->enabled);
  EnableWindow(enabled, trigger != nullptr);
  EnableWindow(remove, trigger != nullptr);
  // Calibration can repair an invalid sidecar by replacing the selected
  // assignment with a newly validated one-rule result.
  EnableWindow(calibrate, editable && !current_path_.empty());
  InvalidateRect(enabled, nullptr, TRUE);
}

bool EditorWindow::save_visual_profile() {
  if (!visual_profile_ || current_path_.empty()) return false;
  // A visual action is an immediate commit. Synchronize and validate the base
  // controls as part of the same bundle so its on-disk ID/cooldowns can never
  // lag the sidecar being published.
  sync_profile_from_controls();
  const auto serialized = layerc::core::serialize_profile_json(profile_);
  const auto parsed = layerc::core::parse_profile_json(serialized);
  const auto *validated = std::get_if<layerc::core::GameProfile>(&parsed);
  if (validated == nullptr) {
    MessageBoxW(dialog_,
                L"Finish the invalid profile field before committing visual recognition.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  profile_ = *validated;
  visual_profile_->profile_id = profile_.id;
  if (!layerc::platform::write_profile_bundle(current_path_, profile_,
                                               visual_profile_)) {
    MessageBoxW(dialog_,
                L"The matching profile and visual sidecar could not be validated and committed as one bundle. The previous consistent bundle remains active.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  visual_sidecar_state_ = layerc::platform::VisualSidecarState::valid;
  visual_sidecar_error_.clear();
  (void)history_.record(profile_, visual_profile_, false);
  history_.rebase(profile_, visual_profile_);
  dirty_ = false;
  KillTimer(dialog_, autosave_timer);
  sync_current_profile_entry();
  notify_profile_committed(current_path_, profile_);
  refresh_visual_controls();
  return true;
}

void EditorWindow::set_visual_assignment_enabled(const bool enabled) {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || !visual_profile_) {
    refresh_visual_controls();
    return;
  }
  auto *trigger = find_trigger(*visual_profile_, cooldown->id);
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
  if (cooldown == nullptr || !visual_profile_) return;
  const auto *existing = find_trigger(*visual_profile_, cooldown->id);
  if (existing == nullptr) return;
  if (MessageBoxW(dialog_,
                  L"Remove visual recognition from this cooldown? Its ordinary input binding is preserved.",
                  L"Remove visual assignment", MB_OKCANCEL | MB_ICONQUESTION) !=
      IDOK)
    return;

  const auto previous_visual = visual_profile_;
  erase_visual_assignment(*visual_profile_, cooldown->id);
  if (save_visual_profile())
    set_status(L"Visual assignment removed; ordinary input remains active.");
  else {
    visual_profile_ = previous_visual;
    refresh_visual_controls();
  }
}

void EditorWindow::calibrate_visual() {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr) return;
  if (profile_.game.global) {
    MessageBoxW(dialog_,
                L"Visual recognition needs a specific matched game window and is unavailable for global profiles.",
                L"Visual calibration", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (current_path_.empty()) {
    MessageBoxW(dialog_, L"Save this new profile before calibrating it.",
                L"Create profile first", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (dirty_ && !flush_autosave()) return;

  auto matching = profile_;
  matching.enabled = true;
  if (edit_target_ != nullptr && IsWindow(edit_target_)) {
    const auto candidate = candidate_for_window(edit_target_);
    if (candidate && layerc::platform::profile_matches_window(*candidate,
                                                              matching)) {
      launch_calibration(edit_target_);
      return;
    }
  }
  awaiting_calibration_target_ = true;
  awaiting_edit_target_ = false;
  show_tasks();
  set_status(L"Select a matching game window, then choose Calibrate Visual.");
}

void EditorWindow::calibrate_selected_task() {
  const auto *candidate = require_selected_task();
  if (candidate == nullptr) return;
  auto matching = profile_;
  matching.enabled = true;
  if (!layerc::platform::profile_matches_window(*candidate, matching)) {
    MessageBoxW(task_dialog_,
                L"That window does not match this profile. Use Selected Task first if you intend to change the matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  awaiting_calibration_target_ = false;
  ShowWindow(task_dialog_, SW_HIDE);
  if (!launch_calibration(candidate->window)) {
    ShowWindow(task_dialog_, SW_SHOWNORMAL);
    SetForegroundWindow(task_dialog_);
    set_status(L"Visual calibration was not started.");
  }
}

bool EditorWindow::launch_calibration(const HWND target) {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || target == nullptr || !IsWindow(target))
    return false;
  if (!layerc::platform::visual_capture_notice_acknowledged()) {
    if (MessageBoxW(
            dialog_,
            L"Visual recognition captures pixels from the selected game window without injecting into or hooking the game. Protected content can appear black, and anti-cheat compatibility cannot be guaranteed. Continue?",
            L"Visual capture notice", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
      return false;
    if (!layerc::platform::set_visual_capture_notice_acknowledged(true)) {
      MessageBoxW(dialog_, L"Could not save the visual capture acknowledgement.",
                  L"LayerE", MB_OK | MB_ICONWARNING);
      return false;
    }
  }

  std::array<wchar_t, 32'768> module_path{};
  const DWORD module_length = GetModuleFileNameW(
      nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (module_length == 0 || module_length >= module_path.size()) return false;
  const auto child =
      std::filesystem::path(std::wstring_view(module_path.data(), module_length))
          .parent_path() /
      L"layercal.exe";
  if (!std::filesystem::exists(child)) {
    MessageBoxW(dialog_, L"layercal.exe is missing beside LayerE.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }

  SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                               .lpSecurityDescriptor = nullptr,
                               .bInheritHandle = TRUE};
  HANDLE request_read{};
  HANDLE request_write{};
  HANDLE response_read{};
  HANDLE response_write{};
  if (CreatePipe(&request_read, &request_write, &security, 0) == FALSE ||
      CreatePipe(&response_read, &response_write, &security, 0) == FALSE) {
    close_handle(request_read);
    close_handle(request_write);
    close_handle(response_read);
    close_handle(response_write);
    MessageBoxW(dialog_, L"Could not create the private calibration channel.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }
  SetHandleInformation(request_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(response_read, HANDLE_FLAG_INHERIT, 0);

  SIZE_T attribute_size{};
  SetLastError(ERROR_SUCCESS);
  const BOOL probed =
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  if (probed != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      attribute_size == 0) {
    close_handle(request_read);
    close_handle(request_write);
    close_handle(response_read);
    close_handle(response_write);
    MessageBoxW(dialog_,
                L"Could not size the restricted calibration launch attributes.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }
  std::vector<std::byte> attribute_storage(attribute_size);
  auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.data());
  HANDLE inherited[]{request_read, response_write};
  const bool attributes_initialized =
      InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) !=
      FALSE;
  if (!attributes_initialized ||
      UpdateProcThreadAttribute(attributes, 0,
                                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                sizeof(inherited), nullptr, nullptr) == FALSE) {
    if (attributes_initialized) DeleteProcThreadAttributeList(attributes);
    close_handle(request_read);
    close_handle(request_write);
    close_handle(response_read);
    close_handle(response_write);
    MessageBoxW(dialog_, L"Could not restrict calibration handle inheritance.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  std::wstring command = L"\"" + child.wstring() +
                         L"\" --request-pipe " +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(
                             request_read)) +
                         L" --response-pipe " +
                         std::to_wstring(reinterpret_cast<std::uintptr_t>(
                             response_write));
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessW(
      child.c_str(), command_line.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
      child.parent_path().c_str(), &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(attributes);
  close_handle(request_read);
  close_handle(response_write);
  if (created == FALSE) {
    close_handle(request_write);
    close_handle(response_read);
    MessageBoxW(dialog_, L"Could not start layercal.exe.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }
  close_handle(process.hThread);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)) == FALSE ||
        AssignProcessToJobObject(job, process.hProcess) == FALSE)
      close_handle(job);
  }

  layerc::core::CalibrationRequest request{
      .target_window = reinterpret_cast<std::uintptr_t>(target),
      .cooldown_id = cooldown->id,
      .profile_json = layerc::core::serialize_profile_json(profile_),
      .visual_json = visual_profile_
                         ? layerc::core::serialize_visual_profile_json(
                               *visual_profile_)
                         : std::string{},
  };
  HANDLE request_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE io_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (request_finished == nullptr || io_finished == nullptr) {
    if (job != nullptr)
      TerminateJobObject(job, ERROR_NOT_ENOUGH_MEMORY);
    else
      TerminateProcess(process.hProcess, ERROR_NOT_ENOUGH_MEMORY);
    WaitForSingleObject(process.hProcess, 2'000);
    close_handle(request_write);
    close_handle(response_read);
    close_handle(process.hProcess);
    close_handle(job);
    close_handle(request_finished);
    close_handle(io_finished);
    MessageBoxW(dialog_, L"Could not monitor the calibration response.",
                L"Calibration unavailable", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto request_payload =
      layerc::core::encode_calibration_request(request);
  layerc::platform::FramedPipeRead response;
  std::atomic_bool write_succeeded{};
  std::string write_error;
  std::thread io([&] {
    const bool wrote = layerc::platform::write_framed_pipe(
        request_write, request_payload,
        layerc::core::max_calibration_packet_bytes, write_error);
    write_succeeded.store(wrote, std::memory_order_release);
    SetEvent(request_finished);
    if (wrote)
      response = layerc::platform::read_framed_pipe(
          response_read, layerc::core::max_calibration_packet_bytes);
    SetEvent(io_finished);
  });

  EnableWindow(dialog_, FALSE);
  constexpr ULONGLONG request_timeout_ms = 5'000ULL;
  constexpr ULONGLONG calibration_timeout_ms = 30ULL * 60ULL * 1'000ULL;
  const ULONGLONG request_deadline =
      GetTickCount64() + request_timeout_ms;
  const ULONGLONG calibration_deadline =
      GetTickCount64() + calibration_timeout_ms;
  bool request_timed_out{};
  bool request_failed{};
  bool calibration_timed_out{};
  while (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
    const DWORD ready = MsgWaitForMultipleObjects(
        1, &process.hProcess, FALSE, 50, QS_ALLINPUT);
    if (ready == WAIT_OBJECT_0 + 1) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    const ULONGLONG now = GetTickCount64();
    const DWORD request_state =
        WaitForSingleObject(request_finished, 0);
    if (request_state == WAIT_OBJECT_0 &&
        !write_succeeded.load(std::memory_order_acquire)) {
      request_failed = true;
    } else if (request_state != WAIT_OBJECT_0 && now >= request_deadline) {
      request_timed_out = true;
    } else if (now >= calibration_deadline) {
      calibration_timed_out = true;
    }
    if (request_failed || request_timed_out || calibration_timed_out) {
      if (job != nullptr)
        TerminateJobObject(job, WAIT_TIMEOUT);
      else
        TerminateProcess(process.hProcess, WAIT_TIMEOUT);
      WaitForSingleObject(process.hProcess, 2'000);
      break;
    }
  }
  EnableWindow(dialog_, TRUE);
  SetForegroundWindow(dialog_);
  close_handle(process.hProcess);
  if (WaitForSingleObject(io_finished, 2'000) != WAIT_OBJECT_0) {
    (void)CancelSynchronousIo(
        reinterpret_cast<HANDLE>(io.native_handle()));
    (void)WaitForSingleObject(io_finished, 2'000);
  }
  close_handle(request_write);
  close_handle(response_read);
  if (io.joinable()) io.join();
  close_handle(request_finished);
  close_handle(io_finished);
  close_handle(job);

  if (request_timed_out) {
    MessageBoxW(dialog_,
                L"LayerCal did not begin reading its bounded request within five seconds and was stopped. The editor remained responsive and no calibration changes were applied.",
                L"Calibration channel timed out", MB_OK | MB_ICONERROR);
    return false;
  }
  if (calibration_timed_out) {
    MessageBoxW(dialog_,
                L"LayerCal exceeded the 30-minute safety limit and was stopped. No calibration changes were applied.",
                L"Calibration timed out", MB_OK | MB_ICONERROR);
    return false;
  }
  if (request_failed ||
      !write_succeeded.load(std::memory_order_acquire)) {
    MessageBoxW(dialog_, to_wide(write_error).c_str(),
                L"Calibration channel failed", MB_OK | MB_ICONERROR);
    return false;
  }
  if (!response) {
    MessageBoxW(dialog_, to_wide(response.error).c_str(),
                L"Calibration did not return a result", MB_OK | MB_ICONERROR);
    return false;
  }
  auto decoded = layerc::core::decode_calibration_response(response.payload);
  if (const auto *error =
          std::get_if<layerc::core::CalibrationProtocolError>(&decoded)) {
    MessageBoxW(dialog_, to_wide(error->message).c_str(),
                L"Invalid calibration response", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto &result = std::get<layerc::core::CalibrationResponse>(decoded);
  if (!result.error.empty()) {
    MessageBoxW(dialog_, to_wide(result.error).c_str(), L"Calibration failed",
                MB_OK | MB_ICONERROR);
    return false;
  }
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
      layerc::core::parse_visual_profile_json(visual_json, profile_);
  const auto *candidate_profile =
      std::get_if<layerc::core::VisualProfile>(&parsed);
  const auto *cooldown = selected_cooldown();
  if (candidate_profile == nullptr || cooldown == nullptr ||
      candidate_profile->rules.size() != 1 ||
      candidate_profile->triggers.size() != 1 ||
      candidate_profile->triggers.front().cooldown_id != cooldown->id) {
    MessageBoxW(dialog_,
                L"LayerCal returned an invalid result. No profile data was changed.",
                L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }

  layerc::core::VisualProfile merged =
      visual_profile_.value_or(layerc::core::VisualProfile{});
  if (visual_sidecar_state_ == layerc::platform::VisualSidecarState::invalid)
    merged = {};
  merged.profile_id = profile_.id;
  merged.sample_rate_hz = candidate_profile->sample_rate_hz;
  merged.quality = candidate_profile->quality;
  auto candidate_rule = candidate_profile->rules.front();
  auto candidate_trigger = candidate_profile->triggers.front();
  auto *old_trigger = find_trigger(merged, cooldown->id);
  std::string old_rule_id;
  if (old_trigger != nullptr) {
    old_rule_id = old_trigger->rule_id;
    const auto references = std::ranges::count_if(
        merged.triggers,
        [&](const auto &trigger) { return trigger.rule_id == old_rule_id; });
    bool update_shared = references <= 1;
    if (references > 1) {
      const int choice = MessageBoxW(
          dialog_,
          L"This visual rule is shared by multiple cooldowns. Choose Yes to update the shared rule, No to clone it for this cooldown, or Cancel to keep everything unchanged.",
          L"Shared visual rule", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (choice == IDCANCEL) return false;
      update_shared = choice == IDYES;
    }
    if (update_shared) {
      candidate_rule.id = old_rule_id;
      const auto old = std::ranges::find(merged.rules, old_rule_id,
                                         &layerc::core::PixelRule::id);
      if (old != merged.rules.end()) *old = candidate_rule;
      candidate_trigger.rule_id = old_rule_id;
    } else {
      if (!ensure_visual_rule_capacity(dialog_, merged)) return false;
      candidate_rule.id = unique_rule_id(candidate_rule.id, merged.rules);
      candidate_trigger.rule_id = candidate_rule.id;
      merged.rules.push_back(candidate_rule);
    }
    *old_trigger = candidate_trigger;
  } else {
    if (!ensure_visual_rule_capacity(dialog_, merged)) return false;
    candidate_rule.id = unique_rule_id(candidate_rule.id, merged.rules);
    candidate_trigger.rule_id = candidate_rule.id;
    merged.rules.push_back(candidate_rule);
    merged.triggers.push_back(candidate_trigger);
  }

  gc_orphan_visual_rule(merged, old_rule_id);

  const auto error = layerc::core::validate_visual_profile(merged, profile_);
  if (!error.message.empty()) {
    MessageBoxW(dialog_, to_wide(error.message).c_str(),
                L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto previous = visual_profile_;
  visual_profile_ = std::move(merged);
  if (!save_visual_profile()) {
    visual_profile_ = previous;
    refresh_visual_controls();
    return false;
  }
  return true;
}

} // namespace layerc::editor
