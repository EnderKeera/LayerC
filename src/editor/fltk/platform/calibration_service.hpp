#pragma once

#include "calibration_result.hpp"
#include "task_list.hpp"
#include "visual_calibration.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::editor {

class ProfileDocument;

// Visual calibration is a platform service. Linux runs the in-process X11
// dialog; Windows runs the bounded external layercal.exe against one
// matching game window. Both accept through the same typed callback.
enum class CalibrationEnd { cancelled, reported };
using CalibrationRunResult = std::variant<AcceptedCalibration, CalibrationEnd>;

struct CalibrationHost {
  std::function<void(StatusKind, const std::string &)> report;
  // Returns the target window, or nullopt after reporting why / opening
  // Detected Tasks. Only platforms that need a target window call it.
  std::function<std::optional<TaskCandidate>()> resolve_target;
  // The native editor window (HWND on Windows, X11 window on Linux), or 0.
  std::uintptr_t owner_window{};
  // Pumps the editor loop for at most timeout; true if the owner closed.
  std::function<bool(std::chrono::milliseconds timeout)> wait;
};

[[nodiscard]] CalibrationRunResult run_platform_calibration(
    const CalibrationHost &host, const ProfileDocument &document,
    const std::string &cooldown_id, CalibrationModel seed,
    const CalibrationAccept &accept,
    std::span<const platform::EncodedTemplateAsset> owned);

// The availability lines the Visual page shows (each ends with '\n').
[[nodiscard]] std::string_view visual_capture_note() noexcept;
[[nodiscard]] std::string_view isolated_runtime_note() noexcept;

} // namespace layerc::editor
