#pragma once

#include "visual_capture_host.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace layerc::daemon {

class VisualCaptureWorker final : public VisualCaptureHost {
public:
  explicit VisualCaptureWorker(HWND window, HANDLE snapshot_event = nullptr);
  ~VisualCaptureWorker() override;
  VisualCaptureWorker(const VisualCaptureWorker &) = delete;
  VisualCaptureWorker &operator=(const VisualCaptureWorker &) = delete;

  void start() override;
  void stop() noexcept override;
  void configure(std::vector<VisualEvaluationRequest> requests,
                 layerc::core::CaptureQuality quality) override;
  void set_excluded_screen_rects(
      std::span<const RECT> rectangles) override;
  [[nodiscard]] VisualCaptureSnapshot snapshot() const override;
  [[nodiscard]] VisualCaptureHostStatus status() const override;
  [[nodiscard]] layerc::core::VisualRuntime runtime() const noexcept override {
    return layerc::core::VisualRuntime::inline_process;
  }

private:
  struct RunState;
  void apply_pending_configuration(RunState &state);
  void evaluate_requests(
      RunState &state, const layerc::capture::CapturePollResult &result,
      layerc::core::TimePoint now);
  void publish_latest(const RunState &state,
                      layerc::capture::CapturePollResult result);
  void run();

  HWND window_{};
  std::atomic<layerc::core::CaptureQuality> quality_{
      layerc::core::CaptureQuality::balanced};
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
