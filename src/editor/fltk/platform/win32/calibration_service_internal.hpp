#pragma once

#include "platform/calibration_service.hpp"

#include <filesystem>

namespace layerc::editor::detail {

[[nodiscard]] CalibrationRunResult run_platform_calibration_with_image(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, const CalibrationAccept &accept,
    const std::filesystem::path &image);

} // namespace layerc::editor::detail
