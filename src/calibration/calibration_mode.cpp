#include "calibration_mode.hpp"

#include <array>
#include <ranges>

namespace layerc::calibration {
namespace {

constexpr std::array kCalibrationModes{
    CalibrationModeSpec{CalibrationMode::average_color,
                        L"Average color near target", true, false, false, false},
    CalibrationModeSpec{CalibrationMode::brightness, L"Brightness threshold",
                        false, true, false, false},
    CalibrationModeSpec{CalibrationMode::coverage, L"Target color coverage",
                        true, false, true, false},
    CalibrationModeSpec{CalibrationMode::template_match, L"Template match",
                        false, false, false, true},
};

} // namespace

std::span<const CalibrationModeSpec> calibration_modes() noexcept {
  return kCalibrationModes;
}

CalibrationMode calibration_mode_from_index(const std::ptrdiff_t index) noexcept {
  if (index < 0 || static_cast<std::size_t>(index) >= kCalibrationModes.size())
    return CalibrationMode::average_color;
  return kCalibrationModes[static_cast<std::size_t>(index)].mode;
}

std::size_t calibration_mode_index(const CalibrationMode mode) noexcept {
  const auto found =
      std::ranges::find(kCalibrationModes, mode, &CalibrationModeSpec::mode);
  return found == kCalibrationModes.end()
             ? 0U
             : static_cast<std::size_t>(found - kCalibrationModes.begin());
}

const CalibrationModeSpec &
calibration_mode_spec(const CalibrationMode mode) noexcept {
  return kCalibrationModes[calibration_mode_index(mode)];
}

} // namespace layerc::calibration
