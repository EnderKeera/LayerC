#pragma once
#include "layerc/core/visual.hpp"
#include "layerc/platform/visual_assets_linux.hpp"
#include <cstdint>
#include <filesystem>
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
} // namespace layerc::editor
