#include "platform/calibration_service.hpp"

#include "calibration_window.hpp"
#include "profile_document.hpp"

#include <utility>

namespace layerc::editor {

CalibrationRunResult run_platform_calibration(
    const CalibrationHost &, const ProfileDocument &document,
    const std::string &, CalibrationModel seed, const CalibrationAccept &accept,
    const std::span<const platform::EncodedTemplateAsset> owned) {
  if (auto accepted = run_x11_calibration(document.path(), std::move(seed), accept, owned))
    return std::move(*accepted);
  return CalibrationEnd::cancelled;
}

std::string_view visual_capture_note() noexcept {
  return "X11 capture uses Composite/Damage, with a bounded full-frame fallback.\n";
}

std::string_view isolated_runtime_note() noexcept {
  return "Unavailable: the isolated visual worker is Windows-only; this setting is retained, never silently weakened.\n";
}

} // namespace layerc::editor
