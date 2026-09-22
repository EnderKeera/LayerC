#include "editor_window.hpp"
#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/framed_pipe.hpp"
#include "layerc/platform/visual_assets.hpp"

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

void close_handle(HANDLE &handle) {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  handle = nullptr;
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
  MessageBoxW(owner, message.c_str(), L"Visual observation limit",
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

std::optional<VisualObservation>
single_observation(const layerc::core::VisualProfile &profile) {
  if (profile.rules.size() + profile.templates.size() != 1) return std::nullopt;
  if (!profile.rules.empty()) return VisualObservation{profile.rules.front()};
  return VisualObservation{profile.templates.front()};
}

} // namespace

void EditorWindow::report_visual_asset_gc(
    const layerc::platform::VisualAssetGcReport &report) {
  if (report.completed()) return;
  std::wstring message = L"Visual asset cleanup was blocked";
  if (!report.blocked_by.empty())
    message += L" at " + report.blocked_by.wstring();
  if (!report.fault.empty()) message += L": " + to_wide(report.fault);
  set_status(message);
}

void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             const std::string_view cooldown_id) {
  std::string old_rule;
  for (const auto &trigger : visual.triggers) {
    if (trigger.cooldown_id == cooldown_id) {
      old_rule = trigger.observation_id;
      break;
    }
  }
  std::erase_if(visual.triggers, [&](const auto &t) {
    return t.cooldown_id == cooldown_id;
  });
  gc_orphan_visual_observation(visual, old_rule);
}

void gc_orphan_visual_observation(layerc::core::VisualProfile &visual,
                                  const std::string_view observation_id) {
  if (observation_id.empty()) return;
  if (std::none_of(visual.triggers.begin(), visual.triggers.end(),
                   [&](const auto &t) {
                     return t.observation_id == observation_id;
                   })) {
    std::erase_if(visual.rules,
                  [&](const auto &r) { return r.id == observation_id; });
    std::erase_if(visual.templates,
                  [&](const auto &state) {
                    return state.id == observation_id;
                  });
  }
}

void EditorWindow::load_current_visual_profile() {
  document_.visual().reset();
  document_.legacy_visual_schema() = false;
  document_.visual_state() = layerc::platform::VisualSidecarState::absent;
  document_.visual_error().clear();
  if (!document_.path().empty()) {
    auto loaded =
        layerc::platform::load_visual_sidecar(document_.path(), document_.profile());
    document_.visual_state() = loaded.state;
    document_.visual() = std::move(loaded.profile);
    document_.visual_error() = std::move(loaded.error.message);
    if (!loaded.source_path.empty()) {
      const auto contents =
          layerc::platform::read_sidecar_text(
              loaded.source_path, layerc::core::max_visual_json_bytes);
      document_.legacy_visual_schema() =
          contents.has_value() && source_uses_legacy_visual_schema(*contents);
    }
  }
  refresh_visual_controls();
}

void EditorWindow::refresh_visual_controls() {
  if (shell_.window() == nullptr) return;
  const HWND summary = GetDlgItem(shell_.window(), IDC_VISUAL_SUMMARY);
  const HWND enabled = GetDlgItem(shell_.window(), IDC_VISUAL_ENABLED);
  const HWND calibrate = GetDlgItem(shell_.window(), IDC_CALIBRATE_VISUAL);
  const HWND remove = GetDlgItem(shell_.window(), IDC_REMOVE_VISUAL);
  const HWND isolated = GetDlgItem(shell_.window(), IDC_VISUAL_ISOLATED);
  set_text(GetDlgItem(shell_.window(), IDC_VISUAL_RUNTIME_STATUS),
           shell_.visual_runtime_status());

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

  std::wstring text = L"Input only";
  if (document_.profile().game.global) {
    text = L"Visual capture is unavailable for global profiles";
  } else if (document_.visual_state() ==
             layerc::platform::VisualSidecarState::invalid) {
    text = L"Invalid visual sidecar — all triggers blocked";
    if (!document_.visual_error().empty())
      text += L": " + to_wide(document_.visual_error());
  } else if (trigger != nullptr) {
    text = visual_mode_label(*trigger);
    if (rule != nullptr && !rule->name.empty())
      text += L" — Pixel: " + to_wide(rule->name);
    else if (template_state != nullptr && !template_state->name.empty())
      text += L" — Template: " + to_wide(template_state->name);
    if (document_.visual_state() ==
        layerc::platform::VisualSidecarState::recovered_backup)
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
    MessageBoxW(shell_.window(),
                L"Finish the invalid profile field before committing visual "
                L"recognition.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  case CommitResult::upgrade_declined:
    return false;
  case CommitResult::blocked_by_sidecar:
    MessageBoxW(shell_.window(),
                L"Repair the invalid automation sidecar before committing "
                L"visual recognition. This prevents an unrelated visual edit "
                L"from silently discarding recovery data.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  case CommitResult::write_failed:
    MessageBoxW(shell_.window(),
                L"The matching profile and visual sidecar could not be validated and committed as one bundle. The previous consistent bundle remains active.",
                L"Visual save failed", MB_OK | MB_ICONERROR);
    return false;
  }
  document_.visual_state() = layerc::platform::VisualSidecarState::valid;
  document_.visual_error().clear();
  (void)document_.history().record(document_.profile(), document_.visual(), document_.automation(), false);
  mark_committed();
  // Collection is deliberately post-commit. LayerCal may already have made a
  // new content-addressed asset durable, and it must not be considered live
  // until the sidecar reference above is durable too.
  report_visual_asset_gc(
      layerc::platform::garbage_collect_unreferenced_visual_assets());
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
  if (MessageBoxW(shell_.window(),
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
    MessageBoxW(shell_.window(),
                L"Visual recognition needs a specific matched game window and is unavailable for global profiles.",
                L"Visual calibration", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (document_.path().empty()) {
    MessageBoxW(shell_.window(), L"Save this new profile before calibrating it.",
                L"Create profile first", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (document_.dirty() && !flush_autosave()) return;

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
    MessageBoxW(task_picker_.window(),
                L"That window does not match this profile. Use Selected Task first if you intend to change the matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (task_picker_.awaiting_calibration())
    task_picker_.clear_request();
  ShowWindow(task_picker_.window(), SW_HIDE);
  if (!launch_calibration(candidate->window)) {
    ShowWindow(task_picker_.window(), SW_SHOWNORMAL);
    SetForegroundWindow(task_picker_.window());
    set_status(L"Visual calibration was not started.");
  }
}

bool EditorWindow::launch_calibration(const HWND target) {
  const auto *cooldown = selected_cooldown();
  if (cooldown == nullptr || target == nullptr || !IsWindow(target))
    return false;
  report_visual_asset_gc(
      layerc::platform::garbage_collect_unreferenced_visual_assets());
  const auto notice = layerc::platform::read_visual_capture_notice_setting();
  if (notice.failed()) {
    MessageBoxW(shell_.window(), L"Could not read the visual capture acknowledgement.",
                L"LayerC Editor", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!notice.value.value_or(false)) {
    if (MessageBoxW(
            shell_.window(),
            L"Visual recognition captures pixels from the selected game window without injecting into or hooking the game. Protected content can appear black, and anti-cheat compatibility cannot be guaranteed. Continue?",
            L"Visual capture notice", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
      return false;
    if (!layerc::platform::write_visual_capture_notice_setting(true)) {
      MessageBoxW(shell_.window(), L"Could not save the visual capture acknowledgement.",
                  L"LayerC Editor", MB_OK | MB_ICONWARNING);
      return false;
    }
  }

  std::vector<wchar_t> module_path(32'768);
  const DWORD module_length = GetModuleFileNameW(
      nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
  if (module_length == 0 || module_length >= module_path.size()) return false;
  const auto child =
      std::filesystem::path(std::wstring_view(module_path.data(), module_length))
          .parent_path() /
      L"layercal.exe";
  if (!std::filesystem::exists(child)) {
    MessageBoxW(shell_.window(),
                L"layercal.exe is missing beside LayerC Editor.",
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
    MessageBoxW(shell_.window(), L"Could not create the private calibration channel.",
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
    MessageBoxW(shell_.window(),
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
    MessageBoxW(shell_.window(), L"Could not restrict calibration handle inheritance.",
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
    MessageBoxW(shell_.window(), L"Could not start layercal.exe.",
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
      .profile_json = layerc::core::serialize_profile_json(document_.profile()),
      .visual_json = document_.visual()
                         ? layerc::core::serialize_visual_profile_json(
                               *document_.visual())
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
    MessageBoxW(shell_.window(), L"Could not monitor the calibration response.",
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

  EnableWindow(shell_.window(), FALSE);
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
  EnableWindow(shell_.window(), TRUE);
  SetForegroundWindow(shell_.window());
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
    MessageBoxW(shell_.window(),
                L"LayerC Calibration did not begin reading its bounded "
                L"request within five seconds and was stopped. The editor "
                L"remained responsive and no calibration changes were "
                L"applied.",
                L"Calibration channel timed out", MB_OK | MB_ICONERROR);
    return false;
  }
  if (calibration_timed_out) {
    MessageBoxW(shell_.window(),
                L"LayerC Calibration exceeded the 30-minute safety limit "
                L"and was stopped. No calibration changes were applied.",
                L"Calibration timed out", MB_OK | MB_ICONERROR);
    return false;
  }
  if (request_failed ||
      !write_succeeded.load(std::memory_order_acquire)) {
    MessageBoxW(shell_.window(), to_wide(write_error).c_str(),
                L"Calibration channel failed", MB_OK | MB_ICONERROR);
    return false;
  }
  if (!response) {
    MessageBoxW(shell_.window(), to_wide(response.error).c_str(),
                L"Calibration did not return a result", MB_OK | MB_ICONERROR);
    return false;
  }
  auto decoded = layerc::core::decode_calibration_response(response.payload);
  if (const auto *error =
          std::get_if<layerc::core::CalibrationProtocolError>(&decoded)) {
    MessageBoxW(shell_.window(), to_wide(error->message).c_str(),
                L"Invalid calibration response", MB_OK | MB_ICONERROR);
    return false;
  }
  const auto &result = std::get<layerc::core::CalibrationResponse>(decoded);
  if (!result.error.empty()) {
    MessageBoxW(shell_.window(), to_wide(result.error).c_str(), L"Calibration failed",
                MB_OK | MB_ICONERROR);
    return false;
  }
  if (result.applied) {
    const bool merged = merge_calibration_result(result.visual_json);
    if (!merged) return false;
    set_status(L"Visual calibration applied and committed.");
    report_visual_asset_gc(
        layerc::platform::garbage_collect_unreferenced_visual_assets());
  } else {
    set_status(L"Visual calibration cancelled; no changes were made.");
    report_visual_asset_gc(
        layerc::platform::garbage_collect_unreferenced_visual_assets());
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
    MessageBoxW(shell_.window(),
                L"LayerC Calibration returned an invalid result. No profile "
                L"data was changed.",
                L"Calibration rejected", MB_OK | MB_ICONERROR);
    return false;
  }

  layerc::core::VisualProfile merged =
      document_.visual().value_or(layerc::core::VisualProfile{});
  if (document_.visual_state() == layerc::platform::VisualSidecarState::invalid)
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
      const int choice = MessageBoxW(
          shell_.window(),
          L"This visual observation is shared by multiple cooldowns. Choose Yes to update the shared observation, No to clone it for this cooldown, or Cancel to keep everything unchanged.",
          L"Shared visual observation", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (choice == IDCANCEL) return false;
      update_shared = choice == IDYES;
    }
    if (update_shared) {
      if (!ensure_observation_capacity(shell_.window(), merged,
                                       candidate_observation,
                                       old_observation_id))
        return false;
      set_observation_id(candidate_observation, old_observation_id);
      replace_observation(merged, old_observation_id, candidate_observation);
      candidate_trigger.observation_id = old_observation_id;
    } else {
      if (!ensure_observation_capacity(shell_.window(), merged,
                                       candidate_observation))
        return false;
      const auto new_id = unique_observation_id(
          observation_id(candidate_observation), merged);
      set_observation_id(candidate_observation, new_id);
      candidate_trigger.observation_id = new_id;
      append_observation(merged, candidate_observation);
    }
    *old_trigger = candidate_trigger;
  } else {
    if (!ensure_observation_capacity(shell_.window(), merged, candidate_observation))
      return false;
    const auto new_id = unique_observation_id(
        observation_id(candidate_observation), merged);
    set_observation_id(candidate_observation, new_id);
    candidate_trigger.observation_id = new_id;
    append_observation(merged, candidate_observation);
    merged.triggers.push_back(candidate_trigger);
  }

  gc_orphan_visual_observation(merged, old_observation_id);

  const auto error = layerc::core::validate_visual_profile(merged, document_.profile());
  if (!error.message.empty()) {
    MessageBoxW(shell_.window(), to_wide(error.message).c_str(),
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
