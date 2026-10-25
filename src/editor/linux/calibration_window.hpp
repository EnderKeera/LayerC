#pragma once

#include "calibration_result.hpp"
#include "visual_calibration.hpp"
#include "layerc/capture/capture.hpp"
#include "layerc/platform/visual_assets_linux.hpp"

#include <FL/Fl_Widget.H>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::editor {

// No store effects occur inside the dialog. Assets and the complete module
// are validated in memory; the caller's Save publishes them after Apply.
class CalibrationPreview final : public Fl_Widget {
public:
  CalibrationPreview(int x, int y, int width, int height);
  void draw() override;
  int handle(int event) override;
  std::shared_ptr<const layerc::capture::CaptureFrame> frame;
  const layerc::core::TemplateImage *mask{};
  layerc::core::NormalizedRegion selection;
  // 0 selects a region, 1 erases mask pixels, 2 restores mask pixels.
  int tool{};
  std::function<void(layerc::core::NormalizedRegion)> selected;
  std::function<void(double, double, bool)> painted;
private:
  [[nodiscard]] layerc::core::NormalizedRegion image_bounds() const;
  double start_x_{}, start_y_{};
  bool dragging_{};
};

// Runs on Apply with the complete typed draft before the dialog closes. A
// returned reason is shown and the dialog keeps the draft open for correction;
// only an accepted value closes the dialog.
using CalibrationAccept =
    std::function<CalibrationAcceptance(const CalibrationDraft &)>;

// Seeded references load from the editor's owned, possibly unpublished bytes
// first, then from the verified store.
[[nodiscard]] std::optional<AcceptedCalibration> run_x11_calibration(
    const std::filesystem::path &profile_path, CalibrationModel model,
    const CalibrationAccept &accept,
    std::span<const layerc::platform::EncodedTemplateAsset> owned = {});

} // namespace layerc::editor
