#include "calibration_mode.hpp"

#include <array>
#include <ranges>

namespace layerc::calibration {
namespace {

constexpr std::array kCalibrationModes{
    CalibrationModeSpec{
        .mode = CalibrationMode::average_color,
        .label = L"Average color near target",
        .color_controls = true,
        .brightness_controls = false,
        .coverage_controls = false,
        .template_controls = false,
    },
    CalibrationModeSpec{
        .mode = CalibrationMode::brightness,
        .label = L"Brightness threshold",
        .color_controls = false,
        .brightness_controls = true,
        .coverage_controls = false,
        .template_controls = false,
    },
    CalibrationModeSpec{
        .mode = CalibrationMode::coverage,
        .label = L"Target color coverage",
        .color_controls = true,
        .brightness_controls = false,
        .coverage_controls = true,
        .template_controls = false,
    },
    CalibrationModeSpec{
        .mode = CalibrationMode::template_match,
        .label = L"Template match",
        .color_controls = false,
        .brightness_controls = false,
        .coverage_controls = false,
        .template_controls = true,
    },
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
