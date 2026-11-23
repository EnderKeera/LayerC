#include "calibration_service_internal.hpp"

#include "calibration_run_internal.hpp"
#include "profile_document.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/settings_store.hpp"

#include <FL/fl_ask.H>

#include <cstdint>
#include <utility>

namespace layerc::editor {
namespace {

CalibrationRunResult run_calibration(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, const CalibrationAccept &accept,
    const std::filesystem::path *image) {
  const auto owner = reinterpret_cast<HWND>(host.owner_window);
  if (!owner || IsWindow(owner) == FALSE || !host.wait) {
    host.report(StatusKind::failure, "Calibration is unavailable: the editor window is not open.");
    return CalibrationEnd::reported;
  }

  const auto target = host.resolve_target();
  if (!target) return CalibrationEnd::reported;
  const auto window = reinterpret_cast<HWND>(target->window);
  if (IsWindow(window) == FALSE) {
    host.report(StatusKind::failure,
                "That window has closed. Refresh Detected Tasks and try again.");
    return CalibrationEnd::reported;
  }

  const auto notice = platform::read_bool_setting(platform::BoolSetting::visual_capture_notice);
  if (notice.failed()) {
    host.report(StatusKind::failure,
                "Could not read the visual capture acknowledgement.");
    return CalibrationEnd::reported;
  }
  if (!notice.value.value_or(false)) {
    fl_message_title("Visual capture notice");
    if (fl_choice("Visual recognition captures pixels from the selected game window without injecting into or hooking the game. Protected content can appear black, and anti-cheat compatibility cannot be guaranteed. Continue?",
                  "Cancel", "Continue", nullptr) != 1)
      return CalibrationEnd::cancelled;
    if (!platform::write_bool_setting(platform::BoolSetting::visual_capture_notice, true)) {
      host.report(StatusKind::failure,
                  "Could not save the visual capture acknowledgement.");
      return CalibrationEnd::reported;
    }
  }

  const core::CalibrationRequest request{
      .target_window = reinterpret_cast<std::uintptr_t>(window),
      .cooldown_id = cooldown_id,
      .profile_json = core::serialize_profile_json(document.profile()),
      .visual_json = document.visual()
                         ? core::serialize_visual_profile_json(*document.visual())
                         : std::string{},
  };
  const CalibrationPump pump = [&](HANDLE, const DWORD milliseconds) -> CalibrationWait {
    return {.cancel = host.wait(std::chrono::milliseconds{milliseconds}), .quit_code = std::nullopt};
  };
  auto session = image ? detail::run_calibration_session_with_image(owner, request, *image, pump)
                       : run_calibration_session(owner, request, pump);
  if (const auto *failure = std::get_if<CalibrationFailure>(&session)) {
    if (const auto text = calibration_failure_text(*failure))
      host.report(StatusKind::failure, text->title + ": " + text->message);
    return CalibrationEnd::reported;
  }
  const auto &response = std::get<core::CalibrationResponse>(session);
  if (!response.applied) {
    host.report(StatusKind::information,
                "Visual calibration cancelled; no changes were made.");
    return CalibrationEnd::reported;
  }
  auto parsed = core::parse_visual_profile_json(response.visual_json, document.profile());
  auto *visual = std::get_if<core::VisualProfile>(&parsed);
  if (!visual) {
    host.report(StatusKind::failure,
                "LayerC Calibration returned an invalid result. No profile data was changed.");
    return CalibrationEnd::reported;
  }
  auto accepted = accept(CalibrationDraft{std::move(*visual), {}});
  if (const auto *reason = std::get_if<std::string>(&accepted)) {
    host.report(StatusKind::failure, "Not applied: " + *reason);
    return CalibrationEnd::reported;
  }
  return std::get<AcceptedCalibration>(std::move(accepted));
}

} // namespace

CalibrationRunResult detail::run_platform_calibration_with_image(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, const CalibrationAccept &accept,
    const std::filesystem::path &image) {
  return run_calibration(host, document, cooldown_id, accept, &image);
}

CalibrationRunResult run_platform_calibration(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, CalibrationModel, const CalibrationAccept &accept,
    std::span<const platform::EncodedTemplateAsset>) {
  return run_calibration(host, document, cooldown_id, accept, nullptr);
}

std::string_view visual_capture_note() noexcept {
  return "Calibration opens LayerC Calibration for one matching game window; capture uses Windows Graphics Capture, with a Desktop Duplication fallback.\n";
}

std::string_view isolated_runtime_note() noexcept {
  return "The isolated visual runtime runs recognition in LayerD's separate, Job-bounded worker.\n";
}

} // namespace layerc::editor
