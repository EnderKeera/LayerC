#pragma once

#include "layerc/capture/window_capture.hpp"
#include "layerc/core/template_matcher.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <string>
#include <vector>

namespace layerc::daemon {

struct VisualAssetSignature {
  bool present{};
  std::uint32_t volume_serial{};
  std::uint64_t file_index{};
  std::uint64_t file_size{};
  std::uint64_t last_write_ticks{};

  bool operator==(const VisualAssetSignature &) const = default;
};

struct VisualTemplateEvaluation {
  layerc::core::TemplateState state;
  // File identity plus metadata detects atomic replacement even when a repair
  // preserves size and timestamp. Cryptographic verification and PNG decoding
  // remain isolated on the worker thread.
  std::vector<VisualAssetSignature> asset_signatures;
  // Populated only in the worker''s private request copy.
  std::shared_ptr<const std::vector<layerc::core::TemplateImage>> references;
  bool assets_available{true};

  bool operator==(const VisualTemplateEvaluation &other) const noexcept {
    return state == other.state && asset_signatures == other.asset_signatures;
  }
};

struct VisualEvaluationRequest {
  std::wstring source_key;
  std::uint8_t sample_rate_hz{10};
  layerc::core::CaptureQuality quality{
      layerc::core::CaptureQuality::balanced};
  std::vector<layerc::core::PixelRule> rules;
  std::vector<VisualTemplateEvaluation> templates;

  bool operator==(const VisualEvaluationRequest &) const = default;
};

struct VisualEvaluationSnapshot {
  std::wstring source_key;
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

class VisualCaptureWorker {
public:
  VisualCaptureWorker(HWND window, layerc::core::CaptureQuality quality,
                      HANDLE snapshot_event = nullptr);
  ~VisualCaptureWorker();
  VisualCaptureWorker(const VisualCaptureWorker &) = delete;
  VisualCaptureWorker &operator=(const VisualCaptureWorker &) = delete;

  void start();
  void stop();
  void configure(std::vector<VisualEvaluationRequest> requests);
  void set_excluded_screen_rects(std::span<const RECT> rectangles);
  [[nodiscard]] VisualCaptureSnapshot snapshot() const;
  [[nodiscard]] HWND window() const noexcept { return window_; }
  [[nodiscard]] layerc::core::CaptureQuality quality() const noexcept {
    return quality_;
  }

private:
  void run();

  HWND window_{};
  layerc::core::CaptureQuality quality_{};
  HANDLE snapshot_event_{}; // non-owning, owned by VisualService
  std::atomic_bool stopping_{};
  std::atomic_uint8_t sample_rate_hz_{10};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::vector<RECT> exclusions_;
  std::uint64_t exclusion_revision_{};
  std::vector<VisualEvaluationRequest> requests_;
  std::uint64_t request_revision_{};
  VisualCaptureSnapshot latest_;
};

} // namespace layerc::daemon
