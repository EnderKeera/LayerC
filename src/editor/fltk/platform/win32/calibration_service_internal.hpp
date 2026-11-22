#pragma once

#include "calibration_run.hpp"
#include "platform/calibration_service.hpp"

#include <filesystem>

class Fl_Window;

namespace layerc::editor::detail {

[[nodiscard]] CalibrationPump fltk_calibration_pump(Fl_Window &owner);

[[nodiscard]] CalibrationRunResult run_platform_calibration_with_image(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, const CalibrationAccept &accept,
    const std::filesystem::path &image);

} // namespace layerc::editor::detail
