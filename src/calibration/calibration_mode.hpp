#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace layerc::calibration {

enum class CalibrationMode : std::uint8_t {
  average_color,
  brightness,
  coverage,
  template_match,
};

struct CalibrationModeSpec {
  CalibrationMode mode;
  std::wstring_view label;
  bool color_controls;
  bool brightness_controls;
  bool coverage_controls;
  bool template_controls;
};

[[nodiscard]] std::span<const CalibrationModeSpec> calibration_modes() noexcept;
[[nodiscard]] CalibrationMode
calibration_mode_from_index(std::ptrdiff_t index) noexcept;
[[nodiscard]] std::size_t
calibration_mode_index(CalibrationMode mode) noexcept;
[[nodiscard]] const CalibrationModeSpec &
calibration_mode_spec(CalibrationMode mode) noexcept;

} // namespace layerc::calibration
