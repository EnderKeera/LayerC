#pragma once

#include "layerc/capture/window_capture.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/platform/visual_assets.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

using VisualAssetSignature = layerc::platform::VisualAssetSignature;

struct VisualTemplateEvaluation {
  layerc::core::TemplateState state;
  std::vector<VisualAssetSignature> asset_signatures;
  // Populated only in the worker's private request copy.
  std::shared_ptr<const std::vector<layerc::core::TemplateImage>> references;
  bool assets_available{true};

  bool operator==(const VisualTemplateEvaluation &other) const noexcept {
    return state == other.state && asset_signatures == other.asset_signatures;
  }
};

struct VisualEvaluationRequest {
  std::string source_key;
  std::uint8_t sample_rate_hz{10};
  layerc::core::CaptureQuality quality{
      layerc::core::CaptureQuality::balanced};
  std::vector<layerc::core::PixelRule> rules;
  std::vector<VisualTemplateEvaluation> templates;

  bool operator==(const VisualEvaluationRequest &) const = default;
};

struct VisualEvaluationSnapshot {
  std::string source_key;
  std::uint64_t sequence{};
  bool missing_template_assets{};
  std::vector<layerc::core::ObservationMeasurement> measurements;
};

struct VisualCaptureSnapshot {
  std::uint64_t sequence{};
  layerc::capture::CapturePollResult result;
  std::string gpu_description;
  std::vector<VisualEvaluationSnapshot> evaluations;
};

struct VisualCaptureHostStatus {
  bool running{};
  bool quarantined{};
  std::uint32_t fault_count{};
  DWORD helper_process_id{};
  std::string last_error;
};

// Presentation-independent roll-up of visual capture health. Format at the
// tooltip / live-channel boundary only.
struct VisualStatusSummary {
  bool invalid_sidecar{};
  bool disabled{};
  bool inactive{};
  bool fallback{};
  bool unavailable{};
  bool missing_assets{};
  bool quarantined{};
  std::string gpu;
};

[[nodiscard]] std::wstring format_visual_status(const VisualStatusSummary &summary);

class VisualCaptureHost {
public:
  virtual ~VisualCaptureHost() = default;
  VisualCaptureHost(const VisualCaptureHost &) = delete;
  VisualCaptureHost &operator=(const VisualCaptureHost &) = delete;

  virtual void start() = 0;
  virtual void stop() noexcept = 0;
  virtual void configure(std::vector<VisualEvaluationRequest> requests,
                         layerc::core::CaptureQuality quality) = 0;
  virtual void
  set_excluded_screen_rects(std::span<const RECT> rectangles) = 0;
  [[nodiscard]] virtual VisualCaptureSnapshot snapshot() const = 0;
  [[nodiscard]] virtual VisualCaptureHostStatus status() const = 0;
  [[nodiscard]] virtual layerc::core::VisualRuntime runtime() const noexcept = 0;
  virtual void maintain() {}
  virtual void retry() {}

protected:
  VisualCaptureHost() = default;
};

} // namespace layerc::daemon
