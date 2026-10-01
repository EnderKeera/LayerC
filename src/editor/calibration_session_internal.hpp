#pragma once

#include "calibration_session.hpp"

#include <filesystem>

namespace layerc::editor::detail {

// The editor always selects its sibling layercal.exe. Tests select only their
// own executable, exercising the same bounded launch and I/O ownership without
// starting live calibration or accessing production data.
[[nodiscard]] std::variant<layerc::core::CalibrationResponse, CalibrationFailure>
run_calibration_session_with_image(HWND owner,
                                  const layerc::core::CalibrationRequest &request,
                                  const std::filesystem::path &image);

} // namespace layerc::editor::detail
