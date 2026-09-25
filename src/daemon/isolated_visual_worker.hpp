#pragma once

#include "isolated_visual_protocol.hpp"

#include "layerc/platform/bounded_helper.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

namespace layerc::daemon {

// Demand-launched helper process: Job-bounded (kill-on-close), package-path
// validated, and authenticated over an owned named pipe. OS ownership only —
// protocol traffic is IsolatedProtocolClient's job.
class IsolatedHelperProcess {
public:
  IsolatedHelperProcess() = default;
  ~IsolatedHelperProcess();
  IsolatedHelperProcess(const IsolatedHelperProcess &) = delete;
  IsolatedHelperProcess &operator=(const IsolatedHelperProcess &) = delete;

  // Launches layerw.exe suspended, binds Job limits, validates package path
  // and session, resumes, and completes the nonce hello. Remains
  // kill-on-close: terminate() or the Job handle teardown ends the helper.
  [[nodiscard]] bool launch(HWND window, const std::atomic_bool &stopping,
                            std::string &error);
  void terminate(DWORD exit_code) noexcept;
  void close() noexcept;
  [[nodiscard]] HANDLE pipe() const noexcept;
  [[nodiscard]] DWORD process_id() const noexcept;

private:
  [[nodiscard]] bool connect_and_authenticate(std::string_view nonce,
                                              std::string &error);

  mutable std::mutex mutex_;
  platform::BoundedHelper child_;
  platform::OwnedHandle pipe_;
};

// Configure / waitSnapshot / snapshot client for one authenticated helper
// pipe. Owns configuration revisions and the shared snapshot validator.
class IsolatedProtocolClient {
public:
  // Returns true when the published configuration revision changed.
  [[nodiscard]] bool apply_requests(std::vector<VisualEvaluationRequest> requests,
                                    layerc::core::CaptureQuality quality);
  [[nodiscard]] bool apply_exclusions(std::span<const RECT> rectangles);
  void clear_snapshot();
  [[nodiscard]] VisualCaptureSnapshot snapshot() const;
  // Runs until stop, pipe failure, or protocol error. Publishes only
  // accepted (non-duplicate) snapshots.
  void run(HANDLE pipe, const std::atomic_bool &stopping,
           std::atomic_uint64_t &last_progress_ms, HANDLE snapshot_event,
           std::string &error);

private:
  // Returns false on pipe/protocol failure (error is filled).
  [[nodiscard]] bool exchange_configuration(
      HANDLE pipe, std::uint64_t &applied_revision,
      std::atomic_uint64_t &last_progress_ms, std::string &error);
  [[nodiscard]] bool poll_snapshot(HANDLE pipe, std::uint64_t &packet_sequence,
                                   std::atomic_uint64_t &last_progress_ms,
                                   HANDLE snapshot_event, std::string &error);

  mutable std::mutex mutex_;
  std::uint64_t configuration_revision_{};
  std::vector<VisualEvaluationRequest> requests_;
  std::vector<RECT> exclusions_;
  layerc::core::CaptureQuality capture_quality_{
      layerc::core::CaptureQuality::balanced};
  VisualCaptureSnapshot latest_;
  IsolatedSnapshotValidator snapshot_validator_;
};

class IsolatedVisualWorker final : public VisualCaptureHost {
public:
  explicit IsolatedVisualWorker(HWND window, HANDLE snapshot_event = nullptr);
  ~IsolatedVisualWorker() override;
  IsolatedVisualWorker(const IsolatedVisualWorker &) = delete;
  IsolatedVisualWorker &operator=(const IsolatedVisualWorker &) = delete;

  void start() override;
  void stop() noexcept override;
  void configure(std::vector<VisualEvaluationRequest> requests,
                 layerc::core::CaptureQuality capture_quality) override;
  void set_excluded_screen_rects(
      std::span<const RECT> rectangles) override;
  void maintain() override;
  void retry() override;

  [[nodiscard]] VisualCaptureSnapshot snapshot() const override;
  [[nodiscard]] VisualCaptureHostStatus status() const override;
  [[nodiscard]] layerc::core::VisualRuntime runtime() const noexcept override {
    return layerc::core::VisualRuntime::isolated;
  }

private:
  bool run_helper(std::string &error);
  void run();

  HWND window_{};
  HANDLE snapshot_event_{}; // non-owning, owned by IsolatedVisualService
  std::atomic_bool stopping_{};
  std::atomic_bool quarantined_{};
  std::atomic_bool running_{};
  std::atomic_uint64_t last_progress_ms_{};
  std::atomic_uint32_t fault_count_{};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  std::string last_error_;
  IsolatedHelperProcess helper_;
  IsolatedProtocolClient client_;
};

} // namespace layerc::daemon
