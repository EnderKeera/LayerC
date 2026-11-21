#pragma once
#include "layerc/core/visual.hpp"
#include "layerc/platform/profile_visual_assets.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace layerc::editor {
struct CalibrationDraft {
  layerc::core::VisualProfile visual;
  std::vector<layerc::core::TemplateImage> images;
};
struct AcceptedCalibration {
  std::uint64_t generation{};
  std::filesystem::path path;
  layerc::core::VisualProfile visual;
  std::vector<layerc::platform::EncodedTemplateAsset> assets;
};
using CalibrationAcceptance = std::variant<AcceptedCalibration, std::string>;

// Runs on Apply with the complete typed draft before the dialog closes. A
// returned reason is shown and the dialog keeps the draft open for correction;
// only an accepted value closes the dialog.
using CalibrationAccept =
    std::function<CalibrationAcceptance(const CalibrationDraft &)>;
} // namespace layerc::editor
