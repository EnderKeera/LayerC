#pragma once

#include "layerc/capture/window_capture.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <string>
#include <vector>

namespace layerc::daemon {

struct VisualEvaluationRequest {
  std::wstring source_key;
  std::uint8_t sample_rate_hz{10};
  layerc::core::CaptureQuality quality{
      layerc::core::CaptureQuality::balanced};
  std::vector<layerc::core::PixelRule> rules;

  bool operator==(const VisualEvaluationRequest &) const = default;
};

struct VisualEvaluationSnapshot {
  std::wstring source_key;
  std::uint64_t sequence{};
  std::vector<layerc::core::PixelRuleMeasurement> measurements;
};

struct VisualCaptureSnapshot {
  std::uint64_t sequence{};
  layerc::capture::CapturePollResult result;
  std::string gpu_description;
  std::vector<VisualEvaluationSnapshot> evaluations;
};

class VisualCaptureWorker {
public:
  VisualCaptureWorker(HWND window, layerc::core::CaptureQuality quality);
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
